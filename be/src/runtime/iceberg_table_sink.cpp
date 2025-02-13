// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#include "runtime/iceberg_table_sink.h"

#include <memory>
#include <sstream>

#include "agent/master_info.h"
#include "client_cache.h"
#include "column/chunk.h"
#include "column/column.h"
#include "column/column_helper.h"
#include "exec/orc_builder.h"
#include "exprs/expr.h"
#include "fs/fs.h"
#include "fs/fs_broker.h"
#include "gen_cpp/FrontendService.h"
#include "gen_cpp/HeartbeatService_types.h"
#include "gen_cpp/MasterService_types.h"
#include "runtime/exec_env.h"
#include "runtime/datetime_value.h"
#include "runtime/runtime_state.h"
#include "util/debug_util.h"
#include "util/runtime_profile.h"
#include "util/time.h"
#include "util/timezone_utils.h"

namespace starrocks {

IcebergTableSink::IcebergTableSink(ObjectPool* pool, const RowDescriptor& row_desc, const std::vector<TExpr>& t_exprs)
        : _state(nullptr),
          _pool(pool),
          _row_desc(row_desc),
          _t_output_expr(t_exprs),
          t_iceberg_sink(nullptr),
          timeout_ms(0) {}

IcebergTableSink::~IcebergTableSink() {
    delete t_iceberg_sink;
}

Status IcebergTableSink::init(const TDataSink& t_sink) {
    RETURN_IF_ERROR(DataSink::init(t_sink));
    t_iceberg_sink = new TIcebergTableSink(t_sink.iceberg_table_sink);
    _tuple_desc_id = t_iceberg_sink->tuple_id;

    // From the thrift expressions create the real exprs.
    RETURN_IF_ERROR(Expr::create_expr_trees(_pool, _t_output_expr, &_output_expr_ctxs));
    return Status::OK();
}

Status IcebergTableSink::prepare(RuntimeState* state) {
    _state = state;

    std::stringstream title;
    title << "IcebergTableSink (frag_id=" << state->fragment_instance_id() << ")";
    RETURN_IF_ERROR(DataSink::prepare(state));

    // profile must add to state's object pool
    _profile = state->obj_pool()->add(new RuntimeProfile("IcebergTableSink"));

    SCOPED_TIMER(_profile->total_time_counter());

    // Prepare the exprs to run.
    RETURN_IF_ERROR(Expr::prepare(_output_expr_ctxs, state));

    // get table's tuple descriptor
    _output_tuple_desc = state->desc_tbl().get_tuple_descriptor(_tuple_desc_id);
    if (_output_tuple_desc == nullptr) {
        LOG(WARNING) << "unknown destination tuple descriptor, id=" << _tuple_desc_id;
        return Status::InternalError("unknown destination tuple descriptor");
    }
    if (!_output_expr_ctxs.empty()) {
        if (_output_expr_ctxs.size() != _output_tuple_desc->slots().size()) {
            LOG(WARNING) << "number of exprs is not same with slots, num_exprs=" << _output_expr_ctxs.size()
                         << ", num_slots=" << _output_tuple_desc->slots().size();
            return Status::InternalError("number of exprs is not same with slots");
        }
        for (int i = 0; i < _output_expr_ctxs.size(); ++i) {
            if (!is_type_compatible(_output_expr_ctxs[i]->root()->type().type,
                                    _output_tuple_desc->slots()[i]->type().type)) {
                LOG(WARNING) << "type of exprs is not match slot's, expr_type="
                             << _output_expr_ctxs[i]->root()->type().type
                             << ", slot_type=" << _output_tuple_desc->slots()[i]->type().type
                             << ", slot_name=" << _output_tuple_desc->slots()[i]->col_name();
                return Status::InternalError("expr's type is not same with slot's");
            }
        }
    }

    return Status::OK();
}

Status IcebergTableSink::open(RuntimeState* state) {
    // Prepare the exprs to run.
    RETURN_IF_ERROR(Expr::open(_output_expr_ctxs, state));
    // open broker
    int query_timeout = state->query_options().query_timeout;
    timeout_ms = query_timeout > 3600 ? 3600000 : query_timeout * 1000;
    return Status::OK();
}

Status IcebergTableSink::send_chunk(RuntimeState* state, vectorized::Chunk* chunk) {
    size_t num_rows = chunk->num_rows();
    if (!_output_expr_ctxs.empty()) {
        _output_chunk = std::make_unique<vectorized::Chunk>();
        for (size_t i = 0; i < _output_expr_ctxs.size(); ++i) {
            ASSIGN_OR_RETURN(ColumnPtr tmp, _output_expr_ctxs[i]->evaluate(chunk));
            ColumnPtr output_column = nullptr;
            if (tmp->only_null()) {
                // Only null column maybe lost type info
                output_column =
                        vectorized::ColumnHelper::create_column(_output_tuple_desc->slots()[i]->type(), true);
            } else {
                // Unpack normal const column
                output_column = vectorized::ColumnHelper::unpack_and_duplicate_const_column(num_rows, tmp);
            }
            DCHECK(output_column != nullptr);
            _output_chunk->append_column(std::move(output_column), _output_tuple_desc->slots()[i]->id());
        }
        chunk = _output_chunk.get();
    } else {
        chunk->reset_slot_id_to_index();
        for (size_t i = 0; i < _output_tuple_desc->slots().size(); ++i) {
            chunk->set_slot_id_to_index(_output_tuple_desc->slots()[i]->id(), i);
        }
    }

    // find partition reference for each row
    num_rows = chunk->num_rows();
    const std::vector<TIcebergTablePartitionColumn>& partitionColumns =
            t_iceberg_sink->tableDescripter.icebergTable.partitionColumns;
    vectorized::Columns partition_columns(partitionColumns.size());
    size_t index = 0;
    cctz::time_zone ctz;
    TimezoneUtils::find_cctz_time_zone(TimezoneUtils::default_time_zone, ctz);
    int64_t offset = TimezoneUtils::to_utc_offset(ctz);
    char datetimeStr[64];
    DateTimeValue datetime;
    for (const auto& partitionColumn : partitionColumns) {
        for (auto slot : _output_tuple_desc->slots()) {
            if (slot->col_name() == partitionColumn.columnName) {
                partition_columns[index] = chunk->get_column_by_slot_id(slot->id());
                index++;
            }
        }
    }
    string partitionKey;
    char format[] = "%Y-%m-%d";
    auto formatLen = strlen(format);
    std::vector<string> partitionKeys;
    std::vector<string *> partitionReferences;
    partitionReferences.assign(num_rows, nullptr);
    for (size_t i = 0; i < num_rows; ++i) {
        partitionKey = "";
        for (auto it=partitionColumns.begin(); it != partitionColumns.end(); it++) {
            auto columnIdx = std::distance(partitionColumns.begin(), it);
            if (it->transform == "day") {
                vectorized::Datum datum = partition_columns[columnIdx]->get(i);
                int64_t timestamp = datum.get_timestamp().to_unix_second() - offset;
                datetime.from_unixtime(timestamp, cctz::utc_time_zone());
                datetime.to_format_string(format, formatLen, datetimeStr);
                partitionKey += it->partitionName + "=" + string(datetimeStr) + "/";
            } else {
                return Status::NotSupported("unsupported transform " + it->transform);
            }
        }
        if (std::find(partitionKeys.begin(), partitionKeys.end(), partitionKey) == partitionKeys.end()) {
            partitionKeys.emplace_back(partitionKey);
        }
        auto pKey = std::find(partitionKeys.begin(), partitionKeys.end(), partitionKey);

        partitionReferences[i] = (string *)pKey->data();
    }

    DCHECK_EQ(chunk->get_slot_id_to_index_map().size(), _output_tuple_desc->slots().size());

    Status status;
    if (partitionKeys.size() == 1) {
        status = write_to_partition(chunk, partitionKeys.at(0));
    } else {
        std::vector<uint32_t> select_idx;
        select_idx.reserve(num_rows);
        for (const auto& partitionKey : partitionKeys) {
            select_idx.clear();
            auto partition = (string *)partitionKey.data();
            for (auto i=0; i < partitionReferences.size(); i++) {
                if (partitionReferences[i] == partition) {
                    select_idx.push_back(i);
                }
            }

            auto _cur_chunk = chunk->clone_empty_with_slot();
            _cur_chunk->append_selective(*chunk, select_idx.data(), 0, select_idx.size());
            status = write_to_partition(_cur_chunk.get(), partitionKey);
            if (!status.ok()) {
                LOG(WARNING) << "file builder add chunk failed, reason:" << status.get_error_msg();
                return status;
            }
        }
    }
    return status;
}

Status IcebergTableSink::write_to_partition(vectorized::Chunk* chunk, const string& partitionKey) {
    Status status = prepare_partition_writer(partitionKey);
    if (!status.ok()) {
        LOG(WARNING) << "open file writer failed, reason:" << status.get_error_msg();
        return status;
    }
    return _partition_writer_map.at(partitionKey)->add_chunk(chunk);
}

Status IcebergTableSink::close(RuntimeState* state, Status exec_status) {
    Expr::close(_output_expr_ctxs, state);
    for (auto& it: _partition_writer_map) {
        Status st = it.second->finish();
        if (!st.ok()) {
            return st;
        }
    }

    if (state->export_output_files().size() == 0) {
        return Status::OK();
    }

    TAddIcebergFilesRequest request;
    request.files = state->export_output_files();
    request.db_id = t_iceberg_sink->db_id;
    request.table_id = t_iceberg_sink->table_id;
    request.__set_iceberg_add_files_rpc_timeout_ms(config::iceberg_add_files_rpc_timeout_ms * 3 / 4);

    LOG(INFO) << "begin to add data files to iceberg table, files:";
    std::stringstream ss;
    for (const auto& filename : request.files) {
        ss << filename << ",";
    }
    LOG(INFO) << ss.str();

    Status status;
    TMasterInfo master_info = get_master_info();
    const TNetworkAddress& master_address = master_info.network_address;
    FrontendServiceConnection client(state->exec_env()->frontend_client_cache(), master_address,
                                     config::iceberg_add_files_rpc_timeout_ms, &status);
    if (!status.ok()) {
        std::stringstream ss;
        ss << "Connect master failed, with address(" << master_address.hostname << ":" << master_address.port << ")";
        LOG(WARNING) << ss.str();
        return status;
    }

    LOG(INFO) << "addIcebergFiles. request is " << apache::thrift::ThriftDebugString(request).c_str();

    TAddIcebergFilesResponse response;
    try {
        try {
            client->addIcebergFiles(response, request);
        } catch (apache::thrift::transport::TTransportException& e) {
            LOG(WARNING) << "Retrying report export tasks status to master(" << master_address.hostname << ":"
                         << master_address.port << ") because: " << e.what();
            status = client.reopen(config::iceberg_add_files_rpc_timeout_ms);
            if (!status.ok()) {
                LOG(WARNING) << "Client reopen failed. with address(" << master_address.hostname << ":"
                             << master_address.port << ")";
                return status;
            }
            client->addIcebergFiles(response, request);
        }
    } catch (apache::thrift::TException& e) {
        // failed when retry.
        // reopen to disable this connection
        client.reopen(config::iceberg_add_files_rpc_timeout_ms);
        std::stringstream ss;
        ss << "Fail to add export files to iceberg table(" << master_address.hostname << ":" << master_address.port
           << "). reason: " << e.what();
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }
    Status addFilesStatus(response.status);
    if (!addFilesStatus.ok()) {
        LOG(WARNING) << "add files to iceberg table failed, reason:" << addFilesStatus.get_error_msg();
        return addFilesStatus;
    }

    LOG(INFO) << "Successfully add data files to iceberg table."
              << " table_id=" << request.table_id;

    return Status::OK();
}

Status IcebergTableSink::gen_file_name(std::string* file_name) {
    if (!t_iceberg_sink->__isset.file_name_prefix) {
        return Status::InternalError("file name prefix is not set");
    }

    std::stringstream file_name_ss;
    file_name_ss << t_iceberg_sink->file_name_prefix << "_";
    TMasterInfo master_info = get_master_info();
    file_name_ss << std::to_string(master_info.backend_id) << "_";
    file_name_ss << UnixMillis();
    // now file-number is 0.
    // <file-name-prefix>_<file-number>.csv.<timestamp>
    const auto& file_format = string(t_iceberg_sink->file_format);
    if (file_format == "orc") {
        file_name_ss << ".orc" ;
    } else {
        return Status::NotSupported("unsupported file format " + file_format);
    }
    *file_name = file_name_ss.str();
    return Status::OK();
}

Status IcebergTableSink::prepare_partition_writer(const std::string& partition_key) {
    auto iter = _partition_writer_map.find(partition_key);
    if (iter != _partition_writer_map.end()) {
        // reopen a new file
        if (_partition_writer_map.at(partition_key)->file_size() >= t_iceberg_sink->bytes_per_file) {
            RETURN_IF_ERROR(_partition_writer_map.at(partition_key)->finish());
            _partition_writer_map.erase(iter);
        } else {
            return Status::OK();
        }
    }

    std::unique_ptr<WritableFile> output_file;
    std::string file_name;
    RETURN_IF_ERROR(gen_file_name(&file_name));
    std::string file_path = t_iceberg_sink->location + "/data/" + partition_key + file_name;
    WritableFileOptions options{.sync_on_close = true, .mode = FileSystem::MUST_CREATE};

    if (t_iceberg_sink->broker_addresses.empty()) {
        return Status::NotFound("no broker found ");
    }
    const TNetworkAddress& broker_addr = t_iceberg_sink->broker_addresses[0];
    BrokerFileSystem fs_broker(broker_addr, t_iceberg_sink->properties, timeout_ms);
    ASSIGN_OR_RETURN(output_file, fs_broker.new_writable_file(options, file_path));

    const auto& file_format = string(t_iceberg_sink->file_format);
    if (file_format == "orc") {
        uint64_t stripe_size = t_iceberg_sink->bytes_per_file;
        while (stripe_size > 64 * 1024 * 1024) {
            stripe_size /= 2;
        }
        ORCBuilderOptions orcBuilderOptions{
                stripe_size,
                64 * 1024,
                orc::CompressionKind_ZLIB,
                orc::CompressionStrategy_SPEED
        };
        std::vector<std::string> column_names;
        std::unique_ptr<FileBuilder> _file_builder = std::make_unique<ORCBuilder>(
                orcBuilderOptions, std::move(output_file), _output_expr_ctxs, _output_tuple_desc, column_names,
                std::move(t_iceberg_sink->tableDescripter.icebergTable.columnAttributes));
        _partition_writer_map.insert({partition_key, std::move(_file_builder)});
    } else {
        return Status::NotSupported("unsupported file format " + file_format);
    }

    _state->add_export_output_file(file_path);
    return Status::OK();
}

} // namespace starrocks

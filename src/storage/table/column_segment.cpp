#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/common/limits.hpp"

#include <cstring>

namespace duckdb {

ColumnSegment::ColumnSegment(DatabaseInstance &db, LogicalType type_p, ColumnSegmentType segment_type, idx_t start, idx_t count)
    : SegmentBase(start, count), db(db), type(move(type_p)), type_size(GetTypeIdSize(type.InternalType())),
	segment_type(segment_type), stats(type, type_size) {
}

ColumnSegment::ColumnSegment(DatabaseInstance &db, LogicalType type_p, ColumnSegmentType segment_type, idx_t start, idx_t count,
                             unique_ptr<BaseStatistics> statistics)
    : SegmentBase(start, count), db(db), type(move(type_p)), type_size(GetTypeIdSize(type.InternalType())),
	segment_type(segment_type), stats(type, type_size, move(statistics)) {
}

void ColumnSegment::InitializeScan(ColumnScanState &state) {
	data->InitializeScan(state);
}

void ColumnSegment::Scan(ColumnScanState &state, idx_t start_row, idx_t scan_count, Vector &result, idx_t result_offset) {
	D_ASSERT(start_row + scan_count <= this->count);
	data->Scan(state, start_row, scan_count, result, result_offset);
}

void ColumnSegment::FetchRow(ColumnFetchState &state, row_t row_id, Vector &result, idx_t result_idx) {
	data->FetchRow(state, row_id - this->start, result, result_idx);
}

} // namespace duckdb

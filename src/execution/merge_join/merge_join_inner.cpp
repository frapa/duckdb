#include "execution/merge_join.hpp"
#include "parser/expression/comparison_expression.hpp"
#include "common/operator/comparison_operators.hpp"
#include "common/vector_operations/vector_operations.hpp"

using namespace duckdb;
using namespace std;

template <class T>
size_t MergeJoinInner::Equality::Operation(MergeInfo &l, MergeInfo &r) {
	if (l.pos >= l.count) {
		return 0;
	}
	assert(l.sel_vector && r.sel_vector);
	auto ldata = (T *)l.v.data;
	auto rdata = (T *)r.v.data;
	size_t result_count = 0;
	while (true) {
		if (r.pos == r.count || operators::LessThan::Operation(ldata[l.sel_vector[l.pos]], rdata[r.sel_vector[r.pos]])) {
			// left side smaller: move left pointer forward
			l.pos++;
			if (l.pos >= l.count) {
				// left side exhausted
				break;
			}
			// we might need to go back on the right-side after going
			// forward on the left side because the new tuple might have
			// matches with the right side
			while (r.pos > 0 && operators::Equals::Operation(ldata[l.sel_vector[l.pos]], rdata[r.sel_vector[r.pos - 1]])) {
				r.pos--;
			}
		} else if (operators::GreaterThan::Operation(ldata[l.sel_vector[l.pos]], rdata[r.sel_vector[r.pos]])) {
			// right side smaller: move right pointer forward
			r.pos++;
		} else {
			// tuples match
			// output tuple
			l.result[result_count] = l.sel_vector[l.pos];
			r.result[result_count] = r.sel_vector[r.pos];
			result_count++;
			// move right side forward
			r.pos++;
			if (result_count == STANDARD_VECTOR_SIZE) {
				// out of space!
				break;
			}
		}
	}
	return result_count;
}

template <class T>
size_t MergeJoinInner::LessThan::Operation(MergeInfo &l, MergeInfo &r) {
	if (r.pos >= r.count) {
		return 0;
	}
	assert(l.sel_vector && r.sel_vector);
	auto ldata = (T *)l.v.data;
	auto rdata = (T *)r.v.data;
	size_t result_count = 0;
	while (true) {
		if (l.pos < l.count && operators::LessThan::Operation(ldata[l.sel_vector[l.pos]], rdata[r.sel_vector[r.pos]])) {
			// left side smaller: found match
			l.result[result_count] = l.sel_vector[l.pos];
			r.result[result_count] = r.sel_vector[r.pos];
			result_count++;
			// move left side forward
			l.pos++;
			if (result_count == STANDARD_VECTOR_SIZE) {
				// out of space!
				break;
			}
		} else {
			// right side smaller or equal, or left side exhausted: move
			// right pointer forward reset left side to start
			l.pos = 0;
			r.pos++;
			if (r.pos == r.count) {
				break;
			}
		}
	}
	return result_count;
}

template <class T>
size_t MergeJoinInner::LessThanEquals::Operation(MergeInfo &l, MergeInfo &r) {
	if (r.pos >= r.count) {
		return 0;
	}
	assert(l.sel_vector && r.sel_vector);
	auto ldata = (T *)l.v.data;
	auto rdata = (T *)r.v.data;
	size_t result_count = 0;
	while (true) {
		if (l.pos < l.count && operators::LessThanEquals::Operation(ldata[l.sel_vector[l.pos]], rdata[r.sel_vector[r.pos]])) {
			// left side smaller: found match
			l.result[result_count] = l.sel_vector[l.pos];
			r.result[result_count] = r.sel_vector[r.pos];
			result_count++;
			// move left side forward
			l.pos++;
			if (result_count == STANDARD_VECTOR_SIZE) {
				// out of space!
				break;
			}
		} else {
			// right side smaller or equal, or left side exhausted: move
			// right pointer forward reset left side to start
			l.pos = 0;
			r.pos++;
			if (r.pos == r.count) {
				break;
			}
		}
	}
	return result_count;
}

INSTANTIATE_MERGEJOIN_TEMPLATES(MergeJoinInner, Equality);
INSTANTIATE_MERGEJOIN_TEMPLATES(MergeJoinInner, LessThan);
INSTANTIATE_MERGEJOIN_TEMPLATES(MergeJoinInner, LessThanEquals);

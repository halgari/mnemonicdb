#include "mnemonicdb/mnemonic_modify.hpp"
#include "mnemonicdb/mnemonic_table_entry.hpp"
#include "mnemonicdb/mnemonic_transaction.hpp"
#include "mnemonicdb/tempid.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

namespace duckdb {

using mnemonicdb::AttributeDef;
using mnemonicdb::EntityId;

//! A sink that changes rows identified by their row id and reports how many it
//! changed
class MnemonicModify : public PhysicalOperator {
public:
	MnemonicModify(PhysicalPlan &physical_plan, vector<LogicalType> types, MnemonicTableEntry &table,
	               idx_t row_id_index, RowIdHandling row_id_handling, idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
	      table(table), row_id_index(row_id_index), row_id_handling(row_id_handling) {
	}

	MnemonicTableEntry &table;
	idx_t row_id_index;
	RowIdHandling row_id_handling;

	struct State : public GlobalSinkState {
		State(ClientContext &context, const vector<LogicalType> &types) : rows(context, types) {
		}
		mutex lock;
		ColumnDataCollection rows;
		idx_t count = 0;
		unordered_set<EntityId> seen;
	};
	//! Each thread buffers its rows; row order does not matter for UPDATE and
	//! DELETE
	struct LocalState : public LocalSinkState {
		LocalState(ClientContext &context, const vector<LogicalType> &types) : rows(context, types) {
		}
		ColumnDataCollection rows;
	};

public:
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override {
		return make_uniq<State>(context, children[0].get().GetTypes());
	}
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override {
		return make_uniq<LocalState>(context.client, children[0].get().GetTypes());
	}
	//! Rows are buffered and changed in Finalize, so the statement never reads
	//! its own writes
	SinkResultType Sink(ExecutionContext &, DataChunk &chunk, OperatorSinkInput &input) const override {
		input.local_state.Cast<LocalState>().rows.Append(chunk);
		return SinkResultType::NEED_MORE_INPUT;
	}
	SinkCombineResultType Combine(ExecutionContext &, OperatorSinkCombineInput &input) const override {
		auto &state = input.global_state.Cast<State>();
		lock_guard<mutex> guard(state.lock);
		state.rows.Combine(input.local_state.Cast<LocalState>().rows);
		return SinkCombineResultType::FINISHED;
	}
	SinkFinalizeType Finalize(Pipeline &, Event &, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override {
		auto &state = input.global_state.Cast<State>();
		auto &transaction = MnemonicTransaction::Get(context, table.catalog);
		lock_guard<mutex> guard(transaction.GetWriteLock());
		MnemonicStatement statement(transaction);
		auto &writer = transaction.GetWriter();
		for (auto &chunk : state.rows.Chunks()) {
			ModifyChunk(state, transaction, writer, chunk);
		}
		auto &def = table.GetTableDef();
		if (def.schema == "sys" && def.id != mnemonicdb::sys::TX) {
			transaction.ReloadSchema(context);
		}
		statement.Commit();
		return SinkFinalizeType::READY;
	}
	void ModifyChunk(State &state, MnemonicTransaction &transaction, mnemonicdb::TxWriter &writer,
	                 DataChunk &chunk) const {
		chunk.Flatten();
		auto &row_ids = chunk.data[row_id_index];
		for (idx_t row = 0; row < chunk.size(); row++) {
			if (FlatVector::IsNull(row_ids, row)) {
				continue;
			}
			auto entity = FlatVector::GetData<uint64_t>(row_ids)[row];
			if (!state.seen.insert(entity).second) {
				if (row_id_handling == RowIdHandling::ERROR) {
					throw InvalidInputException("MnemonicDB: the same row is modified more than once");
				}
				// The first match wins for UPDATE ... FROM; a repeated DELETE of the
				// same row is a no-op
				continue;
			}
			CheckEntity(transaction, entity);
			ModifyRow(transaction, writer, chunk, row, entity);
			state.count++;
		}
	}
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return true;
	}
	bool IsSource() const override {
		return true;
	}

protected:
	void CheckEntity(MnemonicTransaction &transaction, EntityId entity) const {
		auto &def = table.GetTableDef();
		if (def.schema != "sys") {
			return;
		}
		if (def.id == mnemonicdb::sys::TX) {
			if (entity != transaction.CurrentTxEntity()) {
				throw InvalidInputException("MnemonicDB: only the current "
				                            "transaction's sys.tx row can be changed");
			}
			return;
		}
		auto first_user = def.id == mnemonicdb::sys::TABLES ? 16 : 64;
		if (mnemonicdb::LocalOf(entity) < uint64_t(first_user)) {
			throw InvalidInputException("MnemonicDB: built-in schema entities cannot be changed");
		}
	}

	virtual void ModifyRow(MnemonicTransaction &transaction, mnemonicdb::TxWriter &writer, DataChunk &chunk, idx_t row,
	                       EntityId entity) const = 0;

	SourceResultType GetDataInternal(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &) const override {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(sink_state->Cast<State>().count)));
		return SourceResultType::FINISHED;
	}
};

//===--------------------------------------------------------------------===//
// DELETE
//===--------------------------------------------------------------------===//
class MnemonicDelete : public MnemonicModify {
public:
	using MnemonicModify::MnemonicModify;

	string GetName() const override {
		return "MNEMONICDB_DELETE";
	}

protected:
	void ModifyRow(MnemonicTransaction &, mnemonicdb::TxWriter &writer, DataChunk &, idx_t,
	               EntityId entity) const override {
		for (idx_t a = 1; a <= table.GetTableDef().attributes.size(); a++) {
			writer.RetractAll(table.GetAttribute(a), entity);
		}
	}
};

PhysicalOperator &PlanMnemonicDelete(ClientContext &, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                     PhysicalOperator &plan) {
	if (op.return_chunk) {
		throw NotImplementedException("MnemonicDB does not support DELETE ... RETURNING yet");
	}
	auto &table = op.table.Cast<MnemonicTableEntry>();
	if (table.GetTableDef().schema != "main") {
		throw NotImplementedException("MnemonicDB: rows cannot be deleted from sys "
		                              "tables; use DROP / ALTER TABLE");
	}
	auto row_id_index = op.expressions.back()->Cast<BoundReferenceExpression>().Index();
	auto &result = planner.Make<MnemonicDelete>(op.types, table, row_id_index, RowIdHandling::KEEP_FIRST,
	                                            op.estimated_cardinality);
	result.children.push_back(plan);
	return result;
}

//===--------------------------------------------------------------------===//
// UPDATE
//===--------------------------------------------------------------------===//
class MnemonicUpdate : public MnemonicModify {
public:
	MnemonicUpdate(PhysicalPlan &physical_plan, vector<LogicalType> types, MnemonicTableEntry &table,
	               idx_t row_id_index, RowIdHandling row_id_handling, idx_t estimated_cardinality,
	               vector<PhysicalIndex> columns_p, vector<idx_t> value_indexes_p)
	    : MnemonicModify(physical_plan, std::move(types), table, row_id_index, row_id_handling, estimated_cardinality),
	      columns(std::move(columns_p)), value_indexes(std::move(value_indexes_p)) {
	}

	//! Updated physical columns, and where each one's new value is in the input
	//! chunk
	vector<PhysicalIndex> columns;
	vector<idx_t> value_indexes;

	string GetName() const override {
		return "MNEMONICDB_UPDATE";
	}

protected:
	void ModifyRow(MnemonicTransaction &transaction, mnemonicdb::TxWriter &writer, DataChunk &chunk, idx_t row,
	               EntityId entity) const override {
		for (idx_t i = 0; i < columns.size(); i++) {
			auto &vector = chunk.data[value_indexes[i]];
			if (columns[i].index == MNEMONIC_ID_COLUMN_INDEX) {
				BindId(transaction, vector, row, entity);
				continue;
			}
			auto &attr = table.GetAttribute(columns[i].index);
			if (FlatVector::IsNull(vector, row)) {
				writer.RetractAll(attr, entity);
				continue;
			}
			if (!attr.many) {
				writer.Assert(attr, entity, Encode(writer, attr, vector, row));
				continue;
			}
			// The new list replaces the set: retract what is gone, assert what is new
			auto list = FlatVector::GetData<list_entry_t>(vector)[row];
			auto &child = ListVector::GetChild(vector);
			unordered_set<string> wanted;
			for (idx_t c = list.offset; c < list.offset + list.length; c++) {
				if (FlatVector::IsNull(child, c)) {
					throw InvalidInputException("MnemonicDB: cardinality-many column "
					                            "\"%s\" cannot hold NULL elements",
					                            attr.name);
				}
				wanted.insert(Encode(writer, attr, child, c));
			}
			for (auto &datom : writer.CurrentDatoms(entity, attr.Local())) {
				if (wanted.erase(datom.v) == 0) {
					writer.Retract(attr, entity, datom.v);
				}
			}
			for (auto &v : wanted) {
				writer.Assert(attr, entity, v);
			}
		}
	}

private:
	//! The `id` column of an update leaves the row's entity unchanged. Its value
	//! may be the row's own id, NULL, or a tempid, which is then bound to the row
	//! (so ON CONFLICT DO UPDATE resolves tempids like INSERT does).
	static void BindId(MnemonicTransaction &transaction, const Vector &vector, idx_t row, EntityId entity) {
		if (FlatVector::IsNull(vector, row)) {
			return;
		}
		auto id = FlatVector::GetData<uint64_t>(vector)[row];
		if (id == entity) {
			return;
		}
		if (mnemonicdb::IsTempid(id)) {
			auto bound = transaction.FindTempid(id);
			if (bound && *bound != entity) {
				throw ConstraintException("MnemonicDB: tempid resolved to entity %d "
				                          "earlier in this transaction, but "
				                          "this row is entity %d",
				                          *bound, entity);
			}
			transaction.BindTempid(id, entity);
			return;
		}
		throw BinderException("MnemonicDB: the id column cannot be updated");
	}

	static string Encode(mnemonicdb::TxWriter &writer, const AttributeDef &attr, const Vector &vector, idx_t row) {
		if (attr.is_ref) {
			auto id = FlatVector::GetData<uint64_t>(vector)[row];
			if (mnemonicdb::IsTempid(id) || !mnemonicdb::IsWellFormed(id)) {
				throw InvalidInputException("MnemonicDB: %d is not a valid entity ID for column \"%s\"", id, attr.name);
			}
			writer.GetView().GetStore().CheckAllocated(id);
			string v;
			mnemonicdb::PutEntityId(v, id);
			return v;
		}
		return attr.codec.EncodeValue(vector.GetValue(row));
	}
};

PhysicalOperator &PlanMnemonicUpdate(ClientContext &, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                     PhysicalOperator &plan) {
	if (op.return_chunk || op.capture_old_rows) {
		throw NotImplementedException("MnemonicDB does not support UPDATE ... RETURNING yet");
	}
	auto &table = op.table.Cast<MnemonicTableEntry>();
	vector<PhysicalIndex> columns;
	vector<idx_t> value_indexes;
	for (idx_t i = 0; i < op.columns.size(); i++) {
		// `id` may appear: delete-and-insert updates (e.g. of LIST columns) carry
		// every column, and ON CONFLICT DO UPDATE carries the inserted row's id so
		// its tempid can be bound; BindId checks the value
		columns.push_back(op.columns[i]);
		auto &expr = *op.expressions[i];
		value_indexes.push_back(
		    expr.GetExpressionType() == ExpressionType::BOUND_REF ? expr.Cast<BoundReferenceExpression>().Index() : i);
	}
	// Row ids are the last column of the input chunk
	auto row_id_index = plan.types.size() - 1;
	auto &result = planner.Make<MnemonicUpdate>(op.types, table, row_id_index, op.row_id_handling,
	                                            op.estimated_cardinality, std::move(columns), std::move(value_indexes));
	result.children.push_back(plan);
	return result;
}

} // namespace duckdb

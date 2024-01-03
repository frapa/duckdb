#include "duckdb_odbc.hpp"
#include "driver.hpp"
#include "odbc_diagnostic.hpp"
#include "odbc_fetch.hpp"
#include "odbc_utils.hpp"
#include "handle_functions.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/db_instance_cache.hpp"

#include <odbcinst.h>
#include <locale>

using namespace duckdb;
using duckdb::OdbcDiagnostic;
using duckdb::OdbcUtils;
using duckdb::SQLStateType;
using std::string;

SQLRETURN duckdb::FreeHandle(SQLSMALLINT handle_type, SQLHANDLE handle) {
	if (!handle) {
		return SQL_INVALID_HANDLE;
	}

	switch (handle_type) {
	case SQL_HANDLE_DBC: {
		auto *hdl = static_cast<duckdb::OdbcHandleDbc *>(handle);
		delete hdl;
		return SQL_SUCCESS;
	}
	case SQL_HANDLE_DESC: {
		auto *hdl = static_cast<duckdb::OdbcHandleDesc *>(handle);
		hdl->dbc->ResetStmtDescriptors(hdl);
		delete hdl;
		return SQL_SUCCESS;
	}
	case SQL_HANDLE_ENV: {
		auto *hdl = static_cast<duckdb::OdbcHandleEnv *>(handle);
		delete hdl;
		return SQL_SUCCESS;
	}
	case SQL_HANDLE_STMT: {
		auto *hdl = static_cast<duckdb::OdbcHandleStmt *>(handle);
		hdl->dbc->EraseStmtRef(hdl);
		delete hdl;
		return SQL_SUCCESS;
	}
	default:
		return SQL_INVALID_HANDLE;
	}
}

/**
 * @brief Frees a handle
 * https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfreehandle-function?view=sql-server-ver15
 * @param handle_type
 * @param handle
 * @return SQL return code
 */
SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT handle_type, SQLHANDLE handle) {
	return duckdb::FreeHandle(handle_type, handle);
}

/**
 * @brief Allocates a handle
 * https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlallochandle-function?view=sql-server-ver15
 * @param handle_type Can be SQL_HANDLE_ENV, SQL_HANDLE_DBC, SQL_HANDLE_STMT, SQL_HANDLE_DESC
 * @param input_handle Handle to associate with the new handle, if applicable
 * @param output_handle_ptr The new handle
 * @return
 */
SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT handle_type, SQLHANDLE input_handle, SQLHANDLE *output_handle_ptr) {
	switch (handle_type) {
	case SQL_HANDLE_DBC: {
		D_ASSERT(input_handle);

		auto *env = static_cast<duckdb::OdbcHandleEnv *>(input_handle);
		D_ASSERT(env->type == duckdb::OdbcHandleType::ENV);
		*output_handle_ptr = new duckdb::OdbcHandleDbc(env);
		return SQL_SUCCESS;
	}
	case SQL_HANDLE_ENV:
		*output_handle_ptr = new duckdb::OdbcHandleEnv();
		return SQL_SUCCESS;
	case SQL_HANDLE_STMT: {
		D_ASSERT(input_handle);
		auto *dbc = static_cast<duckdb::OdbcHandleDbc *>(input_handle);
		D_ASSERT(dbc->type == duckdb::OdbcHandleType::DBC);
		*output_handle_ptr = new duckdb::OdbcHandleStmt(dbc);
		return SQL_SUCCESS;
	}
	case SQL_HANDLE_DESC: {
		D_ASSERT(input_handle);
		auto *dbc = static_cast<duckdb::OdbcHandleDbc *>(input_handle);
		D_ASSERT(dbc->type == duckdb::OdbcHandleType::DBC);
		*output_handle_ptr = new duckdb::OdbcHandleDesc(dbc);
		return SQL_SUCCESS;
	}
	default:
		return SQL_INVALID_HANDLE;
	}
}

static SQLUINTEGER ExtractMajorVersion(SQLPOINTER value_ptr) {
	// Values like 380 represent version 3.8, here we extract the major version (3 in this case)
	auto full_version = (SQLUINTEGER)(uintptr_t)value_ptr;
	if (full_version > 100) {
		return full_version / 100;
	}
	if (full_version > 10) {
		return full_version / 10;
	}
	return full_version;
}

SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, SQLPOINTER value_ptr,
                                SQLINTEGER string_length) {
	duckdb::OdbcHandleEnv *env = nullptr;
	SQLRETURN ret = ConvertEnvironment(environment_handle, env);
	if (ret != SQL_SUCCESS) {
		return ret;
	}

	switch (attribute) {
	case SQL_ATTR_ODBC_VERSION: {
		auto major_version = ExtractMajorVersion(value_ptr);
		switch (major_version) {
		case SQL_OV_ODBC3:
		case SQL_OV_ODBC2:
			// TODO actually do something with this?
			// auto version = (SQLINTEGER)(uintptr_t)value_ptr;
			env->odbc_version = major_version;
			return SQL_SUCCESS;
		default:
			return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLSetEnvAttr",
			                                   "ODBC version not supported: " + std::to_string(major_version),
			                                   SQLStateType::ST_HY092, "");
		}
	}
	case SQL_ATTR_CONNECTION_POOLING: {
		auto pooling = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value_ptr));
		switch (pooling) {
		case SQL_CP_OFF:
		case SQL_CP_ONE_PER_DRIVER:
		case SQL_CP_ONE_PER_HENV:
			env->connection_pooling = pooling;
			return SQL_SUCCESS;
		default:
			return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLSetConnectAttr",
			                                   "Connection pooling not supported: " + std::to_string(attribute),
			                                   SQLStateType::ST_HY092, "");
		}
	}
	case SQL_ATTR_CP_MATCH:
		return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLSetConnectAttr",
		                                   "Optional feature not implemented.", SQLStateType::ST_HY092, "");
	case SQL_ATTR_OUTPUT_NTS: /* SQLINTEGER */ {
		auto output_nts = static_cast<SQLINTEGER>(reinterpret_cast<intptr_t>(value_ptr));
		if (output_nts == SQL_TRUE) {
			env->output_nts = SQL_TRUE;
			return SQL_SUCCESS;
		}
		return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLSetConnectAttr",
		                                   "Optional feature not implemented.  SQL_ATTR_OUTPUT_NTS must be SQL_TRUE",
		                                   SQLStateType::ST_HY092, "");
	}
	default:
		return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLSetEnvAttr", "Invalid attribute value",
		                                   SQLStateType::ST_HY024, "");
	}
}

SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, SQLPOINTER value_ptr,
                                SQLINTEGER buffer_length, SQLINTEGER *string_length_ptr) {

	if (value_ptr == nullptr) {
		return SQL_SUCCESS;
	}

	auto *env = static_cast<duckdb::OdbcHandleEnv *>(environment_handle);
	if (env->type != duckdb::OdbcHandleType::ENV) {
		return SQL_INVALID_HANDLE;
	}

	switch (attribute) {
	case SQL_ATTR_ODBC_VERSION:
		*static_cast<SQLINTEGER *>(value_ptr) = env->odbc_version;
		break;
	case SQL_ATTR_CONNECTION_POOLING:
		*static_cast<SQLUINTEGER *>(value_ptr) = env->connection_pooling;
		break;
	case SQL_ATTR_OUTPUT_NTS:
		*static_cast<SQLINTEGER *>(value_ptr) = env->output_nts;
		break;
	case SQL_ATTR_CP_MATCH:
		return duckdb::SetDiagnosticRecord(env, SQL_SUCCESS_WITH_INFO, "SQLGetEnvAttr",
		                                   "Optional feature not implemented.", SQLStateType::ST_HYC00, "");
	}
	return SQL_SUCCESS;
}

/**
 * Get the new database name from the DSN string.
 * Otherwise, try to read the database name from odbc.ini
 */
//static void GetDatabaseNameFromDSN(duckdb::OdbcHandleDbc *dbc, SQLCHAR *conn_str, string &new_db_name) {
//	OdbcUtils::SetValueFromConnStr(conn_str, "Database", new_db_name);
//
//	// given preference for the connection attribute
//	if (!dbc->sql_attr_current_catalog.empty() && new_db_name.empty()) {
//		new_db_name = dbc->sql_attr_current_catalog;
//		return;
//	}
//#if defined ODBC_LINK_ODBCINST || defined WIN32
//	if (new_db_name.empty()) {
//		string dsn_name;
//		OdbcUtils::SetValueFromConnStr(conn_str, "DSN", dsn_name);
//		if (!dsn_name.empty()) {
//			const int MAX_DB_NAME = 256;
//			char db_name[MAX_DB_NAME];
//			SQLGetPrivateProfileString(dsn_name.c_str(), "Database", "", db_name, MAX_DB_NAME, "odbc.ini");
//			new_db_name = string(db_name);
//		}
//	}
//#endif
//}

//! The database instance cache, used so that multiple connections to the same file point to the same database object
duckdb::DBInstanceCache instance_cache;

//static SQLRETURN SetConnection(SQLHDBC connection_handle, SQLCHAR *conn_str) {
//	// TODO actually interpret Database in in_connection_string
//	if (!connection_handle) {
//		return SQL_ERROR;
//	}
//	auto *dbc = (duckdb::OdbcHandleDbc *)connection_handle;
//	if (dbc->type != duckdb::OdbcHandleType::DBC) {
//		return SQL_ERROR;
//	}
//
//	// set DSN
//	OdbcUtils::SetValueFromConnStr(conn_str, "DSN", dbc->dsn);
//
//	string db_name;
//	GetDatabaseNameFromDSN(dbc, conn_str, db_name);
//	dbc->SetDatabaseName(db_name);
//	db_name = dbc->GetDatabaseName();
//
//	if (!db_name.empty()) {
//		duckdb::DBConfig config;
//		if (dbc->sql_attr_access_mode == SQL_MODE_READ_ONLY) {
//			config.options.access_mode = duckdb::AccessMode::READ_ONLY;
//		}
//		bool cache_instance = db_name != ":memory:" && !db_name.empty();
//
//		config.SetOptionByName("duckdb_api", "odbc");
//		std::string custom_user_agent;
//		OdbcUtils::SetValueFromConnStr(conn_str, "custom_user_agent", custom_user_agent);
//		if (!custom_user_agent.empty()) {
//			config.SetOptionByName("custom_user_agent", custom_user_agent);
//		}
//
//		dbc->env->db = instance_cache.GetOrCreateInstance(db_name, config, cache_instance);
//	}
//
//	if (!dbc->conn) {
//		dbc->conn = duckdb::make_uniq<duckdb::Connection>(*dbc->env->db);
//		dbc->conn->SetAutoCommit(dbc->autocommit);
//	}
//	return SQL_SUCCESS;
//}

static SQLRETURN ConvertDBCBeforeConnection(SQLHDBC connection_handle, duckdb::OdbcHandleDbc *&dbc) {
	if (!connection_handle) {
		return SQL_INVALID_HANDLE;
	}
	dbc = static_cast<duckdb::OdbcHandleDbc *>(connection_handle);
	if (dbc->type != duckdb::OdbcHandleType::DBC) {
		return SQL_INVALID_HANDLE;
	}
	return SQL_SUCCESS;
}

bool Connect::SetSuccessWithInfo(SQLRETURN ret) {
	if (SQL_SUCCEEDED(ret)) {
		if (ret == SQL_SUCCESS_WITH_INFO) {
			success_with_info = true;
		}
		return true;
	}
	return false;
}

static bool FindSubstrInSubstr(const std::string &s1, const std::string &s2) {
	std::string longest = s1.length() >= s2.length() ? s1 : s2;
	std::string shortest = s1.length() >= s2.length() ? s2 : s1;

	idx_t longest_match = 0;
	for (int i = 0; i < longest.length(); i++) {
		for (int j = 0; j < shortest.length(); j++) {
			if (longest[i] == shortest[j]) {
				idx_t match = 1;
				while (i + match < longest.length() && j + match < shortest.length() &&
				       longest[i + match] == shortest[j + match]) {
					match++;
				}
				if (match > longest_match) {
					longest_match = match;
				}
			}
		}
	}

	if (longest_match > 4) {
		return true;
	}
	return false;
}

bool Connect::FindSimilar(const std::string &input, std::string &match) {
	duckdb::vector<std::string> keys;
	keys.reserve(conn_str_keynames.size());
	for (auto &key_pair : conn_str_keynames) {
		if (input.find(key_pair.second) != std::string::npos || key_pair.second.find(input) != std::string::npos ||
		    FindSubstrInSubstr(input, key_pair.second)) {
			match = key_pair.second;
			return true;
		}
		keys.push_back(key_pair.second);
	}

	auto result = duckdb::StringUtil::TopNLevenshtein(keys, input);
	return false;
}

SQLRETURN Connect::FindMatchingKey(const std::string &input, ODBCConnStrKey &key) {
	for (auto &key_pair : conn_str_keynames) {
		if (key_pair.second == input) {
			key = key_pair.first;
			return SQL_SUCCESS;
		}
	}

	std::string match;
	// If the input doesn't match a keyname, find a similar keyname
	if (FindSimilar(input, match)) {
		// If there is a similar keyname, populate a diagnostic record with a suggestion
		return duckdb::SetDiagnosticRecord(dbc, SQL_SUCCESS_WITH_INFO, "SQLDriverConnect",
		                                   "Invalid keyword: '" + input + "', Did you mean '" + match + "'?",
		                                   SQLStateType::ST_01S09, "");
	}
	return duckdb::SetDiagnosticRecord(dbc, SQL_SUCCESS_WITH_INFO, "SQLDriverConnect", "Invalid keyword",
	                                   SQLStateType::ST_01S09, "");
}

SQLRETURN Connect::FindKeyValPair(const std::string &row) {
	ODBCConnStrKey key;

	size_t val_pos = row.find(KEY_VAL_DEL);
	if (val_pos == std::string::npos) {
		// an equal '=' char must be present (syntax error)
		return (duckdb::SetDiagnosticRecord(dbc, SQL_ERROR, "SQLDriverConnect", "Invalid connection string",
		                                    SQLStateType::ST_HY000, ""));
	}

	SQLRETURN ret = FindMatchingKey(duckdb::StringUtil::Lower(row.substr(0, val_pos)), key);
	if (ret != SQL_SUCCESS) {
		return ret;
	}

	SetVal(key, row.substr(val_pos + 1));
	return SQL_SUCCESS;
}

SQLRETURN Connect::SetVal(ODBCConnStrKey key, const std::string &val) {
	if (CheckSet(key)) {
		return SQL_SUCCESS;
	}
	return (this->*handle_functions.at(key))(val);
}

SQLRETURN Connect::ParseInputStr() {
	size_t row_pos;
	std::string row;

	if (input_str.empty()) {
		return SQL_SUCCESS;
	}

	while ((row_pos = input_str.find(ROW_DEL)) != std::string::npos) {
		row = input_str.substr(0, row_pos);
		SQLRETURN ret = FindKeyValPair(row);
		if (ret != SQL_SUCCESS) {
			return ret;
		}
		input_str.erase(0, row_pos + 1);
	}

	if (input_str.empty()) {
		return SQL_SUCCESS;
	}

	SQLRETURN ret = FindKeyValPair(input_str);
	if (ret != SQL_SUCCESS) {
		return ret;
	}
	return SQL_SUCCESS;
}

SQLRETURN Connect::ReadFromIniFile() {
	duckdb::unique_ptr<duckdb::FileSystem> fs = duckdb::FileSystem::CreateLocal();
	std::string home_directory = fs->GetHomeDirectory();

	std::string odbc_file = home_directory + "/.odbc.ini";

	if (!fs->FileExists(odbc_file)) {
		return SQL_SUCCESS;
	}

	if (dbc->dsn.empty()) {
		return SQL_SUCCESS;
	}

	for (auto &key_pair : conn_str_keynames) {
		if (CheckSet(key_pair.first)) {
			continue;
		}
		const int max_val_len = 256;
		char char_val[max_val_len];
		int read_size = SQLGetPrivateProfileString(OdbcUtils::ConvertStringToLPCSTR(dbc->dsn), key_pair.second.c_str(),
		                                           "", char_val, max_val_len, odbc_file.c_str());
		if (read_size == 0) {
			continue;
		} else if (read_size < 0) {
			return duckdb::SetDiagnosticRecord(dbc, SQL_ERROR, "SQLDriverConnect", "Error reading from .odbc.ini",
			                                   SQLStateType::ST_01S09, "");
		}
		SQLRETURN ret = SetVal(key_pair.first, string(char_val));
		if (ret != SQL_SUCCESS) {
			return ret;
		}
	}
	return SQL_SUCCESS;
}

SQLRETURN Connect::HandleDsn(const string &val) {
	dbc->dsn = val;
	set_keys[DSN] = true;
	return SQL_SUCCESS;
}

SQLRETURN Connect::HandleDatabase(const string &val) {
	std::string new_db_name = val;
	// given preference for the connection attribute
	if (!dbc->sql_attr_current_catalog.empty() && new_db_name.empty()) {
		new_db_name = dbc->sql_attr_current_catalog;
	}

	dbc->SetDatabaseName(new_db_name);
	set_keys[DATABASE] = true;
	return SQL_SUCCESS;
}

SQLRETURN Connect::HandleAllowUnsignedExtensions(const string &val) {
	if (duckdb::StringUtil::Lower(val) == "true") {
		config.options.allow_unsigned_extensions = true;
	}
	set_keys[UNSIGNED] = true;
	return SQL_SUCCESS;
}

SQLRETURN Connect::HandleAccessMode(const string &val) {
	std::string val_lower = duckdb::StringUtil::Lower(val);
	if (val_lower == "read_only") {
		dbc->sql_attr_access_mode = SQL_MODE_READ_ONLY;
	} else if (val_lower == "read_write") {
		dbc->sql_attr_access_mode = SQL_MODE_READ_WRITE;
	} else {
		return duckdb::SetDiagnosticRecord(dbc, SQL_SUCCESS_WITH_INFO, "SQLDriverConnect",
		                                   "Invalid access mode: '" + val +
		                                       "'.  Accepted values are 'READ_ONLY' and 'READ_WRITE'",
		                                   SQLStateType::ST_01S09, "");
	}
	config.options.access_mode = OdbcUtils::ConvertSQLAccessModeToDuckDBAccessMode(dbc->sql_attr_access_mode);
	set_keys[ACCESS_MODE] = true;
	return SQL_SUCCESS;
}

SQLRETURN Connect::HandleCustomUserAgent(const string &val) {
	if (!val.empty()) {
		config.options.custom_user_agent = val;
	}
	set_keys[CUSTOM_USER_AGENT] = true;
	return SQL_SUCCESS;
}

SQLRETURN Connect::SetConnection() {
#if defined ODBC_LINK_ODBCINST || defined WIN32
	ReadFromIniFile();
#endif

	std::string database = dbc->GetDatabaseName();
	config.SetOptionByName("duckdb_api", "odbc");

	bool cache_instance = database != ":memory:" && !database.empty();
	dbc->env->db = instance_cache.GetOrCreateInstance(database, config, cache_instance);

	if (!dbc->conn) {
		dbc->conn = duckdb::make_uniq<duckdb::Connection>(*dbc->env->db);
		dbc->conn->SetAutoCommit(dbc->autocommit);
	}
	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC connection_handle, SQLHWND window_handle, SQLCHAR *in_connection_string,
                                   SQLSMALLINT string_length1, SQLCHAR *out_connection_string,
                                   SQLSMALLINT buffer_length, SQLSMALLINT *string_length2_ptr,
                                   SQLUSMALLINT driver_completion) {
	duckdb::OdbcHandleDbc *dbc = nullptr;
	SQLRETURN ret = ConvertDBCBeforeConnection(connection_handle, dbc);
	if (!SQL_SUCCEEDED(ret)) {
		return ret;
	}

	duckdb::Connect connect(dbc, OdbcUtils::ConvertSQLCHARToString(in_connection_string));

	ret = connect.ParseInputStr();
	if (!connect.SetSuccessWithInfo(ret)) {
		return ret;
	}

	std::string db_name = dbc->GetDatabaseName();

	ret = connect.SetConnection();
	if (!connect.SetSuccessWithInfo(ret)) {
		return ret;
	}

	std::string connect_str = "DuckDB connection";
	if (string_length2_ptr) {
		*string_length2_ptr = connect_str.size();
	}
	if (out_connection_string) {
		memcpy(out_connection_string, connect_str.c_str(),
		       duckdb::MinValue<SQLSMALLINT>((SQLSMALLINT)connect_str.size(), buffer_length));
	}
	return connect.GetSuccessWithInfo() ? SQL_SUCCESS_WITH_INFO : ret;
}

SQLRETURN SQL_API SQLConnect(SQLHDBC connection_handle, SQLCHAR *server_name, SQLSMALLINT name_length1,
                             SQLCHAR *user_name, SQLSMALLINT name_length2, SQLCHAR *authentication,
                             SQLSMALLINT name_length3) {
	duckdb::OdbcHandleDbc *dbc = nullptr;
	SQLRETURN ret = ConvertDBCBeforeConnection(connection_handle, dbc);
	if (!SQL_SUCCEEDED(ret)) {
		return ret;
	}

	duckdb::Connect connect(dbc, OdbcUtils::ConvertSQLCHARToString(server_name));
	connect.HandleDsn(connect.GetInputStr());

	return connect.SetConnection();
}

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number, SQLCHAR *sql_state,
                                SQLINTEGER *native_error_ptr, SQLCHAR *message_text, SQLSMALLINT buffer_length,
                                SQLSMALLINT *text_length_ptr) {

	// lambda function that writes the diagnostic messages
	std::function<bool(duckdb::OdbcHandle *, duckdb::OdbcHandleType)> is_valid_type_func =
	    [&](duckdb::OdbcHandle *hdl, duckdb::OdbcHandleType target_type) {
		    if (hdl->type != target_type) {
			    std::string msg_str("Handle type " + duckdb::OdbcHandleTypeToString(hdl->type) + " mismatch with " +
			                        duckdb::OdbcHandleTypeToString(target_type));
			    OdbcUtils::WriteString(msg_str, message_text, buffer_length, text_length_ptr);
			    return false;
		    }
		    return true;
	    };

	duckdb::OdbcHandle *hdl = nullptr;
	SQLRETURN ret = ConvertHandle(handle, hdl);
	if (ret != SQL_SUCCESS) {
		return ret;
	}

	bool is_valid_type;
	switch (handle_type) {
	case SQL_HANDLE_ENV: {
		is_valid_type = is_valid_type_func(hdl, duckdb::OdbcHandleType::ENV);
		break;
	}
	case SQL_HANDLE_DBC: {
		is_valid_type = is_valid_type_func(hdl, duckdb::OdbcHandleType::DBC);
		break;
	}
	case SQL_HANDLE_STMT: {
		is_valid_type = is_valid_type_func(hdl, duckdb::OdbcHandleType::STMT);
		break;
	}
	case SQL_HANDLE_DESC: {
		is_valid_type = is_valid_type_func(hdl, duckdb::OdbcHandleType::DESC);
		break;
	}
	default:
		return SQL_INVALID_HANDLE;
	}
	if (!is_valid_type) {
		// return SQL_SUCCESS because the error message was written to the message_text
		return SQL_SUCCESS;
	}

	if (rec_number <= 0) {
		OdbcUtils::WriteString("Record number is less than 1", message_text, buffer_length, text_length_ptr);
		return SQL_SUCCESS;
	}
	if (buffer_length < 0) {
		OdbcUtils::WriteString("Buffer length is negative", message_text, buffer_length, text_length_ptr);
		return SQL_SUCCESS;
	}
	if ((size_t)rec_number > hdl->odbc_diagnostic->GetTotalRecords()) {
		return SQL_NO_DATA;
	}

	auto rec_idx = rec_number - 1;
	auto &diag_record = hdl->odbc_diagnostic->GetDiagRecord(rec_idx);

	if (sql_state) {
		OdbcUtils::WriteString(diag_record.sql_diag_sqlstate, sql_state, 6);
	}
	if (native_error_ptr) {
		duckdb::Store<SQLINTEGER>(diag_record.sql_diag_native, (duckdb::data_ptr_t)native_error_ptr);
	}

	std::string msg = diag_record.GetMessage(buffer_length);
	OdbcUtils::WriteString(msg, message_text, buffer_length, text_length_ptr);

	if (text_length_ptr) {
		SQLSMALLINT remaining_chars = msg.size() - buffer_length;
		if (remaining_chars > 0) {
			// TODO needs to split the diagnostic message
			hdl->odbc_diagnostic->AddNewRecIdx(rec_idx);
			return SQL_SUCCESS_WITH_INFO;
		}
	}

	if (message_text == nullptr) {
		return SQL_SUCCESS_WITH_INFO;
	}

	return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                                  SQLSMALLINT diag_identifier, SQLPOINTER diag_info_ptr, SQLSMALLINT buffer_length,
                                  SQLSMALLINT *string_length_ptr) {
	switch (handle_type) {
	case SQL_HANDLE_ENV:
	case SQL_HANDLE_DBC:
	case SQL_HANDLE_STMT:
	case SQL_HANDLE_DESC: {
		duckdb::OdbcHandle *hdl = nullptr;
		SQLRETURN ret = ConvertHandle(handle, hdl);
		if (ret != SQL_SUCCESS) {
			return ret;
		}

		// diag header fields
		switch (diag_identifier) {
		case SQL_DIAG_CURSOR_ROW_COUNT: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::Store<SQLLEN>(hdl->odbc_diagnostic->header.sql_diag_cursor_row_count,
			                      (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_DYNAMIC_FUNCTION: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::OdbcUtils::WriteString(hdl->odbc_diagnostic->GetDiagDynamicFunction(), (SQLCHAR *)diag_info_ptr,
			                               buffer_length, string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_DYNAMIC_FUNCTION_CODE: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::Store<SQLINTEGER>(hdl->odbc_diagnostic->header.sql_diag_dynamic_function_code,
			                          (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_NUMBER: {
			duckdb::Store<SQLINTEGER>(hdl->odbc_diagnostic->header.sql_diag_number, (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_RETURNCODE: {
			duckdb::Store<SQLRETURN>(hdl->odbc_diagnostic->header.sql_diag_return_code,
			                         (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_ROW_COUNT: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::Store<SQLLEN>(hdl->odbc_diagnostic->header.sql_diag_return_code, (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		default:
			break;
		}

		// verify identifier and record index
		if (!OdbcDiagnostic::IsDiagRecordField(diag_identifier)) {
			return SQL_ERROR;
		}
		if (rec_number <= 0) {
			return SQL_ERROR;
		}
		auto rec_idx = rec_number - 1;
		if (!hdl->odbc_diagnostic->VerifyRecordIndex(rec_idx)) {
			return SQL_ERROR;
		}

		auto diag_record = hdl->odbc_diagnostic->GetDiagRecord(rec_idx);

		// diag record fields
		switch (diag_identifier) {
		case SQL_DIAG_CLASS_ORIGIN: {
			duckdb::OdbcUtils::WriteString(hdl->odbc_diagnostic->GetDiagClassOrigin(rec_idx), (SQLCHAR *)diag_info_ptr,
			                               buffer_length, string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_COLUMN_NUMBER: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::Store<SQLINTEGER>(diag_record.sql_diag_column_number, (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_CONNECTION_NAME: {
			// we do not support connection names
			duckdb::OdbcUtils::WriteString("", (SQLCHAR *)diag_info_ptr, buffer_length, string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_MESSAGE_TEXT: {
			auto msg = diag_record.GetMessage(buffer_length);
			duckdb::OdbcUtils::WriteString(msg, (SQLCHAR *)diag_info_ptr, buffer_length, string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_NATIVE: {
			duckdb::Store<SQLINTEGER>(diag_record.sql_diag_native, (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_ROW_NUMBER: {
			// this field is available only for statement handles
			if (hdl->type != duckdb::OdbcHandleType::STMT) {
				return SQL_ERROR;
			}
			duckdb::Store<SQLLEN>(diag_record.sql_diag_row_number, (duckdb::data_ptr_t)diag_info_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_SERVER_NAME: {
			duckdb::OdbcUtils::WriteString(diag_record.sql_diag_server_name, (SQLCHAR *)diag_info_ptr, buffer_length,
			                               string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_SQLSTATE: {
			duckdb::OdbcUtils::WriteString(diag_record.sql_diag_sqlstate, (SQLCHAR *)diag_info_ptr, buffer_length,
			                               string_length_ptr);
			return SQL_SUCCESS;
		}
		case SQL_DIAG_SUBCLASS_ORIGIN: {
			duckdb::OdbcUtils::WriteString(hdl->odbc_diagnostic->GetDiagSubclassOrigin(rec_idx),
			                               (SQLCHAR *)diag_info_ptr, buffer_length, string_length_ptr);
			return SQL_SUCCESS;
		}
		default:
			return SQL_ERROR;
		}
	}
	default:
		return SQL_ERROR;
	}
}

SQLRETURN SQL_API SQLDataSources(SQLHENV environment_handle, SQLUSMALLINT direction, SQLCHAR *server_name,
                                 SQLSMALLINT buffer_length1, SQLSMALLINT *name_length1_ptr, SQLCHAR *description,
                                 SQLSMALLINT buffer_length2, SQLSMALLINT *name_length2_ptr) {
	duckdb::OdbcHandleEnv *env = nullptr;
	SQLRETURN ret = ConvertEnvironment(environment_handle, env);
	if (ret != SQL_SUCCESS) {
		return ret;
	}

	return duckdb::SetDiagnosticRecord(env, SQL_ERROR, "SQLDataSources", "Driver Manager only function",
	                                   SQLStateType::ST_HY000, "");
}

SQLRETURN SQL_API SQLDrivers(SQLHENV environment_handle, SQLUSMALLINT direction, SQLCHAR *driver_description,
                             SQLSMALLINT buffer_length1, SQLSMALLINT *description_length_ptr,
                             SQLCHAR *driver_attributes, SQLSMALLINT buffer_length2,
                             SQLSMALLINT *attributes_length_ptr) {
	duckdb::OdbcHandleEnv *env = nullptr;
	SQLRETURN ret = ConvertEnvironment(environment_handle, env);
	if (ret != SQL_SUCCESS) {
		return ret;
	}

	return duckdb::SetDiagnosticRecord(env, SQL_ERROR, "SQLDrivers", "Driver Manager only function",
	                                   SQLStateType::ST_HY000, "");
}

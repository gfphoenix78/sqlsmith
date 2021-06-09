#include "postgres.hh"
#include "config.h"
#include "expr.hh"
#include <string.h>
#include <iostream>

#ifndef HAVE_BOOST_REGEX
#include <regex>
#else
#include <boost/regex.hpp>
using boost::regex;
using boost::smatch;
using boost::regex_match;
#endif

using namespace std;

static OID public_namespace;
static OID pg_catalog_namespace;
static map<OID, string> oid2namespace;

static regex e_timeout("ERROR:  canceling statement due to statement timeout(\n|.)*");
static regex e_syntax("ERROR:  syntax error at or near(\n|.)*");

static inline bool is_pseudo_type(pg_type *t)
{
  return t->typtype_ == 'p';
}

bool pg_type::consistent(sqltype *rvalue)
{
  pg_type *t = dynamic_cast<pg_type*>(rvalue);

  if (!t) {
    cerr << "unknown type: " << rvalue->name  << endl;
    return false;
  }

  switch(typtype_) {
  case 'b': /* base type */
  case 'c': /* composite type */
  case 'd': /* domain */
  case 'r': /* range */
  case 'e': /* enum */
    return this == t;

  case 'p':
    if (name == "anyarray") {
      return t->typcategory_ == 'A';
    } else if (name == "anynonarray") {
      return t->typcategory_ != 'A';
    } else if(name == "anyenum") {
      return t->typtype_ == 'e';
    } else if (name == "any") {
      return true;
    } else if (name == "anyelement") {
//      return t->typcategory_ == 'A' && t->typelem_ == this->oid_;
      return false;
    } else if (name == "anyrange") {
      return t->typtype_ == 'r';
    } else if (name == "record") {
      return t->typtype_ == 'c';
    } else if (name == "cstring") {
      return this == t;
    } else {
      return false;
    }

  default:
    throw std::logic_error("unknown typtype");
  }
}

string pg_type::fullName() const
{
  auto ns = typnamespace_;
  if (ns == public_namespace || ns == pg_catalog_namespace)
    return name;
  return oid2namespace[ns] + "." + name;
}
          

dut_pqxx::dut_pqxx(std::string conninfo)
  : c(conninfo)
{
     c.set_variable("statement_timeout", "'1s'");
     c.set_variable("client_min_messages", "'ERROR'");
     c.set_variable("application_name", "'" PACKAGE "::dut'");
}

void dut_pqxx::test(const std::string &stmt)
{
  try {
    if(!c.is_open())
       c.activate();

    pqxx::work w(c);
    w.exec(stmt.c_str());
    w.abort();
  } catch (const pqxx::failure &e) {
    if ((dynamic_cast<const pqxx::broken_connection *>(&e))) {
      /* re-throw to outer loop to recover session. */
      throw dut::broken(e.what());
    }

    if (regex_match(e.what(), e_timeout))
      throw dut::timeout(e.what());
    else if (regex_match(e.what(), e_syntax))
      throw dut::syntax(e.what());
    else
      throw dut::failure(e.what());
  }
}


schema_pqxx::schema_pqxx(std::string &conninfo, bool no_catalog) : c(conninfo)
{
  c.set_variable("application_name", "'" PACKAGE "::schema'");

  pqxx::work w(c);
  pqxx::result r = w.exec("select version()");
  version = r[0][0].as<string>();

  r = w.exec("SHOW server_version_num");
  version_num = r[0][0].as<int>();

  supported_features["tablesample"] = version_num >= 90500;
  supported_features["merge_into"] = false;
  // address the schema change in postgresql 11 that replaced proisagg and proiswindow with prokind
  string procedure_is_aggregate = version_num < 110000 ? "proisagg" : "prokind = 'a'";
  string procedure_is_window = version_num < 110000 ? "proiswindow" : "prokind = 'w'";

  cerr << "Loading namespaces...";
  r = w.exec("select oid, nspname from pg_namespace");
  for (auto row = r.begin(); row != r.end(); ++row) {
    OID oid(row[0].as<OID>());
    string name(row[1].as<string>());
    if (name == "public")
      public_namespace = oid;
    else if (name == "pg_catalog")
      pg_catalog_namespace = oid;
    oid2namespace[oid] = name;
  }
  cerr << "done." << endl;

  cerr << "Loading types...";

  r = w.exec("select quote_ident(typname), oid, typdelim, typrelid, typelem, typarray, typtype, typcategory, typnamespace "
             "from pg_type ");

  for (auto row = r.begin(); row != r.end(); ++row) {
    string name(row[0].as<string>());
    OID oid(row[1].as<OID>());
    OID typnamespace(row[8].as<OID>());
    string typdelim(row[2].as<string>());
    OID typrelid(row[3].as<OID>());
    OID typelem(row[4].as<OID>());
    OID typarray(row[5].as<OID>());
    string typtype(row[6].as<string>());
    string typcategory(row[7].as<string>());
    //       if (schema == "pg_catalog")
    // 	continue;
    //       if (schema == "information_schema")
    // 	continue;
    if (name == "unknown")
      continue;

    pg_type *t = new pg_type(name,oid, typnamespace, typdelim[0],typrelid, typelem, typarray, typtype[0], typcategory[0]);
    oid2type[oid] = t;
    name2type[name] = t;
    types.push_back(t);
  }

  booltype = name2type["bool"];
  inttype = name2type["int4"];

  internaltype = name2type["internal"];
  arraytype = name2type["anyarray"];

  cerr << "done." << endl;

  // load range types
  cerr << "Loading range types...";
  r = w.exec("select tp.oid, r.rngsubtype from pg_type as tp left join pg_range as r "
             "on tp.oid=r.rngtypid where tp.typtype='r'");
  for (auto row = r.begin(); row != r.end(); ++row) {
    OID range_type(row[0].as<OID>());
    OID sub_type(row[1].as<OID>());
    oid2type[range_type]->typelem_ = sub_type;
  }
  cerr << "done." << endl;


  cerr << "Loading tables...";
  r = w.exec("select table_name, "
              "table_schema, "
              "is_insertable_into, "
              "table_type "
               "from information_schema.tables");

  for (auto row = r.begin(); row != r.end(); ++row) {
    string schema(row[1].as<string>());
    string insertable(row[2].as<string>());
    string table_type(row[3].as<string>());

    if (no_catalog && ((schema == "pg_catalog") || (schema == "information_schema")))
      continue;

    tables.push_back(table(row[0].as<string>(),
                     schema,
                     ((insertable == "YES") ? true : false),
                     ((table_type == "BASE TABLE") ? true : false)));
  }

  cerr << "done." << endl;

  cerr << "Loading columns and constraints...";

  for (auto t = tables.begin(); t != tables.end(); ++t) {
    string q("select attname, "
             "atttypid "
             "from pg_attribute join pg_class c on( c.oid = attrelid ) "
             "join pg_namespace n on n.oid = relnamespace "
             "where not attisdropped "
             "and attnum > 0");
    q += " and relname = " + w.quote(t->name);
    q += " and nspname = " + w.quote(t->schema);

    r = w.exec(q);
    for (auto row : r) {
      column c(row[0].as<string>(), oid2type[row[1].as<OID>()]);
      t->columns().push_back(c);
    }

    q = "select conname from pg_class t "
        "join pg_constraint c on (t.oid = c.conrelid) "
        "where contype in ('f', 'u', 'p') ";
    q += " and relnamespace = " " (select oid from pg_namespace where nspname = " + w.quote(t->schema) + ")";
    q += " and relname = " + w.quote(t->name);

    for (auto row : w.exec(q)) {
      t->constraints.push_back(row[0].as<string>());
    }

  }
  cerr << "done." << endl;

  cerr << "Loading operators...";

  r = w.exec("select oprname, oprleft,"
             "oprright, oprresult "
             "from pg_catalog.pg_operator "
             "where 0 not in (oprresult, oprright, oprleft)");
  for (auto row : r) {
    op o(row[0].as<string>(),
    oid2type[row[1].as<OID>()],
    oid2type[row[2].as<OID>()],
    oid2type[row[3].as<OID>()]);
    register_operator(o);
  }

  cerr << "done." << endl;

  cerr << "Loading routines...";
  r = w.exec("select (select nspname from pg_namespace where oid = pronamespace), oid, prorettype, proname "
             "from pg_proc "
             "where prorettype::regtype::text not in ('event_trigger', 'trigger', 'opaque', 'internal') "
             "and proname <> 'pg_event_trigger_table_rewrite_reason' "
             "and proname <> 'pg_event_trigger_table_rewrite_oid' "
             "and proname !~ '^ri_fkey_' "
             "and proname !~ '^unknown' "
             "and not (proretset or " + procedure_is_aggregate + " or " + procedure_is_window + ") ");

  for (auto row : r) {
    routine proc(row[0].as<string>(),
                 row[1].as<string>(),
                 oid2type[row[2].as<long>()],
                 row[3].as<string>());
    register_routine(proc);
  }

  cerr << "done." << endl;

  cerr << "Loading routine parameters...";

  for (auto &proc : routines) {
    string q("select unnest(proargtypes) "
             "from pg_proc ");
    q += " where oid = " + w.quote(proc.specific_name);

    r = w.exec(q);
    for (auto row : r) {
      sqltype *t = oid2type[row[0].as<OID>()];
      assert(t);
      proc.argtypes.push_back(t);
    }
  }
  cerr << "done." << endl;

  cerr << "Loading aggregates...";
  r = w.exec("select (select nspname from pg_namespace where oid = pronamespace), oid, prorettype, proname "
             "from pg_proc "
             "where prorettype::regtype::text not in ('event_trigger', 'trigger', 'opaque', 'internal') "
             "and proname not in ('pg_event_trigger_table_rewrite_reason') "
             "and proname not in ('percentile_cont', 'dense_rank', 'cume_dist', "
             "'rank', 'test_rank', 'percent_rank', 'percentile_disc', 'mode', 'test_percentile_disc') "
             "and proname !~ '^ri_fkey_' "
             "and not (proretset or " + procedure_is_window + ") "
             "and " + procedure_is_aggregate);

  for (auto row : r) {
    routine proc(row[0].as<string>(),
                 row[1].as<string>(),
                 oid2type[row[2].as<OID>()],
                 row[3].as<string>());
    register_aggregate(proc);
  }

  cerr << "done." << endl;

  cerr << "Loading aggregate parameters...";

  for (auto &proc : aggregates) {
    string q("select unnest(proargtypes) "
             "from pg_proc ");
    q += " where oid = " + w.quote(proc.specific_name);

    r = w.exec(q);
    for (auto row : r) {
      sqltype *t = oid2type[row[0].as<OID>()];
      assert(t);
      proc.argtypes.push_back(t);
    }
  }
  cerr << "done." << endl;
  c.disconnect();
  generate_indexes();
}

bool schema_pqxx::support_feature(const std::string &feature_name)
{
  return supported_features[feature_name];
}

extern "C" {
    void dut_libpq_notice_rx(void *arg, const PGresult *res);
}

void dut_libpq_notice_rx(void *arg, const PGresult *res)
{
    (void) arg;
    (void) res;
}

void dut_libpq::connect(std::string &conninfo)
{
    if (conn) {
        PQfinish(conn);
    }
    conn = PQconnectdb(conninfo.c_str());
    char *errmsg = PQerrorMessage(conn);
    if (strlen(errmsg))
       throw dut::broken(errmsg, "08001");

    command("set statement_timeout to '1s'");
    command("set client_min_messages to 'ERROR';");
    command("set application_name to '" PACKAGE "::dut';");

    PQsetNoticeReceiver(conn, dut_libpq_notice_rx, (void *) 0);
}

dut_libpq::dut_libpq(std::string conninfo)
    : conninfo_(conninfo)
{
    connect(conninfo);
}

void dut_libpq::command(const std::string &stmt)
{
    if (!conn)
        connect(conninfo_);
    PGresult *res = PQexec(conn, stmt.c_str());

    switch (PQresultStatus(res)) {

    case PGRES_FATAL_ERROR:
    default:
    {
        const char *errmsg = PQresultErrorMessage(res);
        if (!errmsg || !strlen(errmsg))
             errmsg = PQerrorMessage(conn);

        const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
        if (!sqlstate || !strlen(sqlstate))
             sqlstate =  (CONNECTION_OK != PQstatus(conn)) ? "08000" : "?????";

        std::string error_string(errmsg);
        std::string sqlstate_string(sqlstate);
        PQclear(res);

        if (CONNECTION_OK != PQstatus(conn)) {
                  PQfinish(conn);
            conn = 0;
            throw dut::broken(error_string.c_str(), sqlstate_string.c_str());
        }
        if (sqlstate_string == "42601")
             throw dut::syntax(error_string.c_str(), sqlstate_string.c_str());
        else
             throw dut::failure(error_string.c_str(), sqlstate_string.c_str());
    }

    case PGRES_NONFATAL_ERROR:
    case PGRES_TUPLES_OK:
    case PGRES_SINGLE_TUPLE:
    case PGRES_COMMAND_OK:
        PQclear(res);
        return;
    }
}

void dut_libpq::test(const std::string &stmt)
{
    command("ROLLBACK;");
    command("BEGIN;");
    command(stmt.c_str());
    command("ROLLBACK;");
}

bool comparison_op::Init()
{
  int n_retry = 20;

retry:
  pg_type *left, *right;
  pg_type *ptype_left, *ptype_right;
  auto &idx = this->scope->schema->operators_returning_type;

  auto iters = idx.equal_range(scope->schema->booltype);
  oper = random_pick<>(iters)->second;
  left = dynamic_cast<pg_type*>(oper->left);
  right = dynamic_cast<pg_type*>(oper->right);
  assert(left && right);

  lhs = value_expr::factory(this, oper->left);
  rhs = value_expr::factory(this, oper->right);

  if (oper->left == oper->right) {
    if (lhs->type == rhs->type)
      return true;

    if (lhs->type->consistent(rhs->type))
      lhs = value_expr::factory(this, rhs->type);
    else
      rhs = value_expr::factory(this, lhs->type);
    if (lhs->type != rhs->type)
      goto fail;
    return true;
  }
  return true;

  ptype_left = dynamic_cast<pg_type*>(lhs->type);
  ptype_right = dynamic_cast<pg_type*>(rhs->type);
  assert(ptype_left && ptype_right && ptype_left != ptype_right);
//  if (ptype_left->name == "text" || ptype_right->name == "text")
    cerr << "\nFound text type: (" << left->name << ", "
         << right->name << ") get (" << ptype_left->name << ", "
         << ptype_right << ")" << endl;

  if (left->name == "anyrange" || right->name == "anyrange") {
    // the other must be anyelement
    assert(left->name == "anyelement" || right->name == "anyelement");
    if ((ptype_left->typtype_ == 'r' && ptype_left->typelem_ == ptype_right->oid_) ||
        (ptype_right->typtype_ == 'r' && ptype_right->typelem_ == ptype_left->oid_))
      return true;
  } else if (left->name == "anyarray" || right->name == "anyarray") {
    assert(left->name == "anyelement" || right->name == "anyelement");
    if ((ptype_left->typcategory_ == 'A' && ptype_left->typelem_ == ptype_right->oid_) ||
        (ptype_right->typcategory_ == 'A' && ptype_right->typelem_ == ptype_left->oid_))
      return true;
  } else {
  }

fail:
  if (--n_retry >= 0)
    goto retry;
  throw std::runtime_error("Can't build comparision:");
}

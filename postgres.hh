/// @file
/// @brief schema and dut classes for PostgreSQL

#ifndef POSTGRES_HH
#define POSTGRES_HH

#include "dut.hh"
#include "relmodel.hh"
#include "schema.hh"

#include <pqxx/pqxx>

extern "C" {
#include <libpq-fe.h>
}

#define OID long

struct pg_type : sqltype {
  OID oid_;
  OID typnamespace_;
  OID typrelid_;
  OID typelem_;
  OID typarray_;
  char typtype_;
  char typcategory_;
  char typdelim_;
  pg_type(string name,
  OID oid,
  OID typnamespace,
  char typdelim,
  OID typrelid,
  OID typelem,
  OID typarray,
  char typtype,
  char typcategory)
    : sqltype(name), oid_(oid), typnamespace_(typnamespace), typrelid_(typrelid), typelem_(typelem), typarray_(typarray),
      typtype_(typtype), typcategory_(typcategory), typdelim_(typdelim) { }

  virtual bool consistent(struct sqltype *rvalue);
  virtual string fullName() const override;
  bool consistent_(sqltype *rvalue);
};


struct schema_pqxx : public schema {
  pqxx::connection c;
  map<OID, pg_type*> oid2type;
  map<string, pg_type*> name2type;
  map<std::string, bool> supported_features;

  virtual std::string quote_name(const std::string &id) {
    return c.quote_name(id);
  }
  virtual bool support_feature(const std::string &feature_name) override;
  schema_pqxx(std::string &conninfo, bool no_catalog);
};

struct dut_pqxx : dut_base {
  pqxx::connection c;
  virtual void test(const std::string &stmt);
  dut_pqxx(std::string conninfo);
};

struct dut_libpq : dut_base {
     PGconn *conn = 0;
     std::string conninfo_;
     virtual void test(const std::string &stmt);
     void command(const std::string &stmt);
     void connect(std::string &conninfo);
     dut_libpq(std::string conninfo);
};

#endif

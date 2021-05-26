/// @file
/// @brief logging

#ifndef LOG_HH
#define LOG_HH

#include <exception>
#include <pqxx/pqxx>
#include <string>

#include "prod.hh"
#include "dut.hh"

void loadKnownErrors(const std::string &filename);
bool isKnownError(const std::string &error);
bool isKnownError(const char *error);

/// logger base class
struct logger {
  virtual void generated(prod &query) {(void)query; }
  virtual void executed(prod &query) {(void)query; }
  virtual void error(prod &query, const dut::failure &e) {
    (void)query, (void)e;
  }
  virtual void known_error(prod &query, const dut::failure &e) {
    (void)query, (void)e;
  }
  bool dispatch_error(prod &query, const dut::failure &e);
};

/// logger to dump all generated queries
struct query_dumper : logger {
  virtual void generated(prod &query) {
       query.out(std::cout);
       std::cout << ";" << std::endl;
  }
};

/// logger for statistics collection
struct stats_collecting_logger : logger {
  long queries = 0;
  float sum_nodes = 0;
  float sum_height = 0;
  float sum_retries = 0;
  virtual void generated(prod &query);
};

/// stderr logger
struct cerr_logger : stats_collecting_logger {
  const int columns = 80;
  std::map<std::string, long> errors;
  std::map<std::string, long> known_errors;
  virtual void report();
  virtual void generated(prod &query);
  virtual void executed(prod &query);
  virtual void error(prod &query, const dut::failure &e);
  virtual void known_error(prod &query, const dut::failure &e);
  void report(prod &p);
};

/// logger to postgres database
struct pqxx_logger : stats_collecting_logger {
  long id;
  std::shared_ptr<pqxx::connection> c;
  pqxx_logger(std::string target, std::string conninfo, struct schema &s);
  virtual void generated(prod &query);
  virtual void error(prod &query, const dut::failure &e);
  virtual void known_error(prod &query, const dut::failure &e);
};

#endif

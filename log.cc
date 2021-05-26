#include "config.h"
#include <iostream>
#include <pqxx/pqxx>
#include <sstream>

#ifndef HAVE_BOOST_REGEX
#include <regex>
#else
#include <boost/regex.hpp>
using boost::regex;
using boost::smatch;
using boost::regex_match;
#endif

#include <string>
#include <unordered_set>

extern "C" {
#include <string.h>
#include <unistd.h>
}

#include "log.hh"
#include "schema.hh"
#include "gitrev.h"
#include "impedance.hh"
#include "random.hh"

using namespace std;
using namespace pqxx;

std::unordered_set<std::string> known_errors;

void loadKnownErrors(const std::string &filename)
{
    std::cout << "Loading known errors...";
    char line[2048];
    FILE *file = fopen(filename.c_str(), "r");
    if (!file) {
        std::cout << "Failed to open '" << filename << "': ";
        std::cout << strerror(errno) << std::endl;
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        auto len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0)
            known_errors.insert(line);
    }
    fclose(file);
    std::cout << "done." << std::endl;

#if 0
    std::cout << "Dump known errors:" << std::endl;
    for (auto it = known_errors.begin(); it != known_errors.end(); ++it)
        std::cout << "\t'" << *it << "'\n";
#endif
}
bool isKnownError(const std::string &error)
{
    return known_errors.find(error) != known_errors.end();
}
bool isKnownError(const char *error)
{
    return known_errors.find(error) != known_errors.end();
}

// returns true if the error is unknown
bool logger::dispatch_error(prod &query, const dut::failure &e)
{
  std::istringstream err(e.what());
  std::string line;
  getline(err, line);
  auto known = isKnownError(line.substr(0, 80));
  if (known)
      this->known_error(query, e);
  else
      this->error(query, e);
  return !known;
}

struct stats_visitor : prod_visitor {
  int nodes = 0;
  int maxlevel = 0;
  long retries = 0;
  map<const char*, long> production_stats;
  virtual void visit(struct prod *p) {
    nodes++;
    if (p->level > maxlevel)
      maxlevel = p->level;
    production_stats[typeid(*p).name()]++;
    retries += p->retries;
  }
  void report() {
    cerr << "production statistics" << endl;
    vector<pair<const char *, long> > report;
    for (auto p : production_stats)
      report.push_back(p);
    stable_sort(report.begin(), report.end(),
		[](const pair<std::string, long> &a,
		   const pair<std::string, long> &b)
		{ return a.second > b.second; });
    for (auto p : report) {
      cerr << p.second << "\t" << p.first << endl;
    }
  }
};

void stats_collecting_logger::generated(prod &query)
{
  queries++;
  
  stats_visitor v;
  query.accept(&v);

  sum_nodes += v.nodes;
  sum_height += v.maxlevel;
  sum_retries += v.retries;
}

void cerr_logger::report()
{
    cerr << endl << "queries: " << queries << endl;
// 	 << " (" << 1000.0*query_count/gen_time.count() << " gen/s, "
// 	 << 1000.0*query_count/query_time.count() << " exec/s)" << endl;
    cerr << "AST stats (avg): height = " << sum_height/queries
	 << " nodes = " << sum_nodes/queries << endl;

    vector<pair<std::string, long> > report;
    for (auto e : errors) {
      report.push_back(e);
    }
    stable_sort(report.begin(), report.end(),
		[](const pair<std::string, long> &a,
		   const pair<std::string, long> &b)
		{ return a.second > b.second; });
    long err_count = 0;
    for (auto e : report) {
      err_count += e.second;
      cerr << e.second << "\t'" << e.first.substr(0,80) << "'" << endl;
    }
    cerr << "error rate: " << (float)err_count/(queries) << endl;
    impedance::report();

}


void cerr_logger::generated(prod &p)
{
  stats_collecting_logger::generated(p);
  if ((10*columns-1) == queries%(10*columns))
    report();
}

void cerr_logger::executed(prod &query)
{
  (void)query;
  if (columns-1 == (queries%columns)) {
    cerr << endl;
  }
  cerr << ".";
}

void cerr_logger::error(prod &query, const dut::failure &e)
{
  (void)query;
  istringstream err(e.what());
  string line;

  if (columns-1 == (queries%columns)) {
    cerr << endl;
  }
  getline(err, line);
  errors[line]++;
  if (dynamic_cast<const dut::timeout *>(&e))
    cerr << "t";
  else if (dynamic_cast<const dut::syntax *>(&e))
    cerr << "S";
  else if (dynamic_cast<const dut::broken *>(&e))
    cerr << "C";
  else
    cerr << "e";
}

void cerr_logger::known_error(prod &query, const dut::failure &e)
{
  (void)query;
  (void)e;
  if (columns-1 == (queries%columns)) {
    cerr << endl;
  }
  cerr << "K";

}

pqxx_logger::pqxx_logger(std::string target, std::string conninfo, struct schema &s)
{
  c = make_shared<pqxx::connection>(conninfo);

  work w(*c);
  w.exec("set application_name to '" PACKAGE "::log';");

  c->prepare("instance",
	     "insert into instance (rev, target, hostname, version, seed) "
	     "values ($1, $2, $3, $4, $5) returning id");

  char hostname[1024];
  gethostname(hostname, sizeof(hostname));

  ostringstream seed;
  seed << smith::rng;
    
  result r = w.prepared("instance")(GITREV)(target)(hostname)(s.version)(seed.str()).exec();
  
  id = r[0][0].as<long>(id);

  c->prepare("error",
	     "insert into error (id, msg, query, sqlstate) "
	     "values (" + to_string(id) + ", $1, $2, $3)");

  w.exec("insert into stat (id) values (" + to_string(id) + ")");
  c->prepare("stat",
	     "update stat set generated=$1, level=$2, nodes=$3, updated=now() "
	     ", retries = $4, impedance = $5 "
	     "where id = " + to_string(id));

  w.commit();

}

void pqxx_logger::error(prod &query, const dut::failure &e)
{
  work w(*c);
  ostringstream s;
  s << query;
  w.prepared("error")(e.what())(s.str())(e.sqlstate).exec();
  w.commit();
}

void pqxx_logger::known_error(prod &query, const dut::failure &e)
{
  // TODO: log known errors
  (void)query;
  (void)e;
}

void pqxx_logger::generated(prod &query)
{
  stats_collecting_logger::generated(query);
  if (999 == (queries%1000)) {
    work w(*c);
    ostringstream s;
    impedance::report(s);
    w.prepared("stat")(queries)(sum_height/queries)(sum_nodes/queries)(sum_retries/queries)(s.str()).exec();
    w.commit();
  }
}


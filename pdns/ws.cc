/*
    Copyright (C) 2002 - 2012  PowerDNS.COM BV

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2 
    as published by the Free Software Foundation

    Additionally, the license of this program contains a special
    exception which allows to distribute the program in binary form when
    it is linked against OpenSSL.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "utility.hh"
#include "dynlistener.hh"
#include "ws.hh"
#include "json.hh"
#include "webserver.hh"
#include "logger.hh"
#include "packetcache.hh"
#include "statbag.hh"
#include "misc.hh"
#include "arguments.hh"
#include "dns.hh"
#include "ueberbackend.hh"
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include "namespaces.hh"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "version.hh"

using namespace rapidjson;

extern StatBag S;

typedef map<string,string> varmap_t;

StatWebServer::StatWebServer()
{
  d_start=time(0);
  d_min10=d_min5=d_min1=0;
  d_ws = 0;
  d_tid = 0;
  if(arg().mustDo("webserver"))
    d_ws = new WebServer(arg()["webserver-address"], arg().asNum("webserver-port"),arg()["webserver-password"]);
}

void StatWebServer::go()
{
  if(arg().mustDo("webserver"))
  {
    S.doRings();
    pthread_create(&d_tid, 0, threadHelper, this);
    pthread_create(&d_tid, 0, statThreadHelper, this);
  }
}

void StatWebServer::statThread()
{
  try {
    for(;;) {
      d_queries.submit(S.read("udp-queries"));
      d_cachehits.submit(S.read("packetcache-hit"));
      d_cachemisses.submit(S.read("packetcache-miss"));
      d_qcachehits.submit(S.read("query-cache-hit"));
      d_qcachemisses.submit(S.read("query-cache-miss"));
      Utility::sleep(1);
    }
  }
  catch(...) {
    L<<Logger::Error<<"Webserver statThread caught an exception, dying"<<endl;
    exit(1);
  }
}

void *StatWebServer::statThreadHelper(void *p)
{
  StatWebServer *sws=static_cast<StatWebServer *>(p);
  sws->statThread();
  return 0; // never reached
}


void *StatWebServer::threadHelper(void *p)
{
  StatWebServer *sws=static_cast<StatWebServer *>(p);
  sws->launch();
  return 0; // never reached
}

static string htmlescape(const string &s) {
  string result;
  for(string::const_iterator it=s.begin(); it!=s.end(); ++it) {
    switch (*it) {
    case '&':
      result += "&amp;";
      break;
    case '<':
      result += "&lt;";
      break;
    case '>':
      result += "&gt;";
      break;
    default:
      result += *it;
    }
  }
  return result;
}

void printtable(ostringstream &ret, const string &ringname, const string &title, int limit=10)
{
  int tot=0;
  int entries=0;
  vector<pair <string,unsigned int> >ring=S.getRing(ringname);

  for(vector<pair<string, unsigned int> >::const_iterator i=ring.begin(); i!=ring.end();++i) {
    tot+=i->second;
    entries++;
  }

  ret<<"<div class=\"panel\">";
  ret<<"<span class=resetring><i></i><a href=\"?resetring="<<ringname<<"\">Reset</a></span>"<<endl;
  ret<<"<h2>"<<title<<"</h2>"<<endl;
  ret<<"<div class=ringmeta>";
  ret<<"<a class=topXofY href=\"?ring="<<ringname<<"\">Showing: Top "<<limit<<" of "<<entries<<"</a>"<<endl;
  ret<<"<span class=resizering>Resize: ";
  unsigned int sizes[]={10,100,500,1000,10000,500000,0};
  for(int i=0;sizes[i];++i) {
    if(S.getRingSize(ringname)!=sizes[i])
      ret<<"<a href=\"?resizering="<<ringname<<"&amp;size="<<sizes[i]<<"\">"<<sizes[i]<<"</a> ";
    else
      ret<<"("<<sizes[i]<<") ";
  }
  ret<<"</span></div>";

  ret<<"<table class=\"data\">";
  int printed=0;
  int total=max(1,tot);
  for(vector<pair<string,unsigned int> >::const_iterator i=ring.begin();limit && i!=ring.end();++i,--limit) {
    ret<<"<tr><td>"<<htmlescape(i->first)<<"</td><td>"<<i->second<<"</td><td align=right>"<< StatWebServer::makePercentage(i->second*100.0/total)<<"</td>"<<endl;
    printed+=i->second;
  }
  ret<<"<tr><td colspan=3></td></tr>"<<endl;
  if(printed!=tot)
    ret<<"<tr><td><b>Rest:</b></td><td><b>"<<tot-printed<<"</b></td><td align=right><b>"<< StatWebServer::makePercentage((tot-printed)*100.0/total)<<"</b></td>"<<endl;

  ret<<"<tr><td><b>Total:</b></td><td><b>"<<tot<<"</b></td><td align=right><b>100%</b></td>";
  ret<<"</table></div>"<<endl;
}

void StatWebServer::printvars(ostringstream &ret)
{
  ret<<"<div class=panel><h2>Variables</h2><table class=\"data\">"<<endl;

  vector<string>entries=S.getEntries();
  for(vector<string>::const_iterator i=entries.begin();i!=entries.end();++i) {
    ret<<"<tr><td>"<<*i<<"</td><td>"<<S.read(*i)<<"</td><td>"<<S.getDescrip(*i)<<"</td>"<<endl;
  }

  ret<<"</table></div>"<<endl;
}

void StatWebServer::printargs(ostringstream &ret)
{
  ret<<"<table border=1><tr><td colspan=3 bgcolor=\"#0000ff\"><font color=\"#ffffff\">Arguments</font></td>"<<endl;

  vector<string>entries=arg().list();
  for(vector<string>::const_iterator i=entries.begin();i!=entries.end();++i) {
    ret<<"<tr><td>"<<*i<<"</td><td>"<<arg()[*i]<<"</td><td>"<<arg().getHelp(*i)<<"</td>"<<endl;
  }
}

string StatWebServer::makePercentage(const double& val)
{
  return (boost::format("%.01f%%") % val).str();
}

void StatWebServer::indexfunction(HttpRequest* req, HttpResponse* resp)
{
  if(!req->parameters["resetring"].empty()) {
    if (S.ringExists(req->parameters["resetring"]))
      S.resetRing(req->parameters["resetring"]);
    resp->status = 301;
    resp->headers["Location"] = "/";
    return;
  }
  if(!req->parameters["resizering"].empty()){
    int size=atoi(req->parameters["size"].c_str());
    if (S.ringExists(req->parameters["resizering"]) && size > 0 && size <= 500000)
      S.resizeRing(req->parameters["resizering"], atoi(req->parameters["size"].c_str()));
    resp->status = 301;
    resp->headers["Location"] = "/";
    return;
  }

  ostringstream ret;

  ret<<"<!DOCTYPE html>"<<endl;
  ret<<"<html><head>"<<endl;
  ret<<"<title>PowerDNS Authoritative Server Monitor</title>"<<endl;
  ret<<"<link rel=\"stylesheet\" href=\"style.css\"/>"<<endl;
  ret<<"</head><body>"<<endl;

  ret<<"<div class=\"row\">"<<endl;
  ret<<"<div class=\"headl columns\">";
  ret<<"<a href=\"/\" id=\"appname\">PowerDNS "VERSION;
  if(!arg()["config-name"].empty()) {
    ret<<" ["<<arg()["config-name"]<<"]";
  }
  ret<<"</a></div>"<<endl;
  ret<<"<div class=\"headr columns\"></div></div>";
  ret<<"<div class=\"row\"><div class=\"all columns\">";

  time_t passed=time(0)-s_starttime;

  ret<<"<p>Uptime: "<<
    humanDuration(passed)<<
    "<br>"<<endl;

  ret<<"Queries/second, 1, 5, 10 minute averages:  "<<std::setprecision(3)<<
    d_queries.get1()<<", "<<
    d_queries.get5()<<", "<<
    d_queries.get10()<<". Max queries/second: "<<d_queries.getMax()<<
    "<br>"<<endl;
  
  if(d_cachemisses.get10()+d_cachehits.get10()>0)
    ret<<"Cache hitrate, 1, 5, 10 minute averages: "<<
      makePercentage((d_cachehits.get1()*100.0)/((d_cachehits.get1())+(d_cachemisses.get1())))<<", "<<
      makePercentage((d_cachehits.get5()*100.0)/((d_cachehits.get5())+(d_cachemisses.get5())))<<", "<<
      makePercentage((d_cachehits.get10()*100.0)/((d_cachehits.get10())+(d_cachemisses.get10())))<<
      "<br>"<<endl;

  if(d_qcachemisses.get10()+d_qcachehits.get10()>0)
    ret<<"Backend query cache hitrate, 1, 5, 10 minute averages: "<<std::setprecision(2)<<
      makePercentage((d_qcachehits.get1()*100.0)/((d_qcachehits.get1())+(d_qcachemisses.get1())))<<", "<<
      makePercentage((d_qcachehits.get5()*100.0)/((d_qcachehits.get5())+(d_qcachemisses.get5())))<<", "<<
      makePercentage((d_qcachehits.get10()*100.0)/((d_qcachehits.get10())+(d_qcachemisses.get10())))<<
      "<br>"<<endl;

  ret<<"Backend query load, 1, 5, 10 minute averages: "<<std::setprecision(3)<<
    d_qcachemisses.get1()<<", "<<
    d_qcachemisses.get5()<<", "<<
    d_qcachemisses.get10()<<". Max queries/second: "<<d_qcachemisses.getMax()<<
    "<br>"<<endl;

  ret<<"Total queries: "<<S.read("udp-queries")<<". Question/answer latency: "<<S.read("latency")/1000.0<<"ms</p><br>"<<endl;
  if(req->parameters["ring"].empty()) {
    vector<string>entries=S.listRings();
    for(vector<string>::const_iterator i=entries.begin();i!=entries.end();++i)
      printtable(ret,*i,S.getRingTitle(*i));

    printvars(ret);
    if(arg().mustDo("webserver-print-arguments"))
      printargs(ret);
  }
  else
    printtable(ret,req->parameters["ring"],S.getRingTitle(req->parameters["ring"]),100);

  ret<<"</div></div>"<<endl;
  ret<<"<footer class=\"row\">"<<fullVersionString()<<"<br>&copy; 2013 <a href=\"http://www.powerdns.com/\">PowerDNS.COM BV</a>.</footer>"<<endl;
  ret<<"</body></html>"<<endl;

  resp->body = ret.str();
}

static int intFromJson(const Value& val) {
  if (val.IsInt()) {
    return val.GetInt();
  } else if (val.IsString()) {
    return atoi(val.GetString());
  } else {
    throw PDNSException("Value not an Integer");
  }
}

static string getZone(const string& zonename) {
  UeberBackend B;
  DomainInfo di;
  if(!B.getDomainInfo(zonename, di))
    return returnJSONError("Could not find domain '"+zonename+"'");

  Document doc;
  doc.SetObject();

  Value root;
  root.SetObject();
  root.AddMember("name", zonename.c_str(), doc.GetAllocator());
  root.AddMember("type", "Zone", doc.GetAllocator());
  root.AddMember("kind", di.getKindString(), doc.GetAllocator());
  Value masters;
  masters.SetArray();
  BOOST_FOREACH(const string& master, di.masters) {
    Value value(master.c_str(), doc.GetAllocator());
    masters.PushBack(value, doc.GetAllocator());
  }
  root.AddMember("masters", masters, doc.GetAllocator());
  root.AddMember("serial", di.serial, doc.GetAllocator());
  root.AddMember("notified_serial", di.notified_serial, doc.GetAllocator());
  root.AddMember("last_check", (unsigned int) di.last_check, doc.GetAllocator());

  DNSResourceRecord rr;
  Value records;
  records.SetArray();
  di.backend->list(zonename, di.id);
  while(di.backend->get(rr)) {
    if (!rr.qtype.getCode())
      continue; // skip empty non-terminals

    Value object;
    object.SetObject();
    Value jname(rr.qname.c_str(), doc.GetAllocator()); // copy
    object.AddMember("name", jname, doc.GetAllocator());
    Value jtype(rr.qtype.getName().c_str(), doc.GetAllocator()); // copy
    object.AddMember("type", jtype, doc.GetAllocator());
    object.AddMember("ttl", rr.ttl, doc.GetAllocator());
    object.AddMember("priority", rr.priority, doc.GetAllocator());
    Value jcontent(rr.content.c_str(), doc.GetAllocator()); // copy
    object.AddMember("content", jcontent, doc.GetAllocator());
    records.PushBack(object, doc.GetAllocator());
  }
  root.AddMember("records", records, doc.GetAllocator());

  doc.AddMember("zone", root, doc.GetAllocator());
  return makeStringFromDocument(doc);
}

static string createOrUpdateZone(const string& zonename, bool onlyCreate, varmap_t& varmap) {
  UeberBackend B;
  DomainInfo di;

  bool exists = B.getDomainInfo(zonename, di);
  if(exists && onlyCreate)
    return returnJSONError("Domain '"+zonename+"' already exists");

  if(!exists) {
    if(!B.createDomain(zonename))
      return returnJSONError("Creating domain '"+zonename+"' failed");

    if(!B.getDomainInfo(zonename, di))
      return returnJSONError("Creating domain '"+zonename+"' failed: lookup of domain ID failed");

    // create SOA record so zone "really" exists
    DNSResourceRecord soa;
    soa.qname = zonename;
    soa.content = "1";
    soa.qtype = "SOA";
    soa.domain_id = di.id;
    soa.auth = 0;
    soa.ttl = ::arg().asNum( "default-ttl" );
    soa.priority = 0;

    di.backend->startTransaction(zonename, di.id);
    di.backend->feedRecord(soa);
    di.backend->commitTransaction();
  }

  di.backend->setKind(zonename, DomainInfo::stringToKind(varmap["kind"]));
  di.backend->setMaster(zonename, varmap["master"]);

  return getZone(zonename);
}

static void apiServerConfig(HttpRequest* req, HttpResponse* resp) {
  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  vector<string> items = ::arg().list();
  Document doc;
  doc.SetArray();
  BOOST_FOREACH(const string& var, items) {
    Value kv, key, value;
    kv.SetArray();
    key.SetString(var.c_str(), var.length());
    kv.PushBack(key, doc.GetAllocator());

    if(var.find("password") != string::npos)
      value="*****";
    else
      value.SetString(::arg()[var].c_str(), ::arg()[var].length(), doc.GetAllocator());

    kv.PushBack(value, doc.GetAllocator());
    doc.PushBack(kv, doc.GetAllocator());
  }
  resp->body = makeStringFromDocument(doc);
}

static void apiServerSearchLog(HttpRequest* req, HttpResponse* resp) {
  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  resp->body = makeLogGrepJSON(req->parameters["q"], ::arg()["experimental-logfile"], " pdns[");
}

static void apiServerZones(HttpRequest* req, HttpResponse* resp) {
  if(req->method != "GET")
    throw HttpMethodNotAllowedException();

  UeberBackend B;
  vector<DomainInfo> domains;
  B.getAllDomains(&domains);

  Document doc;
  doc.SetObject();

  Value jdomains;
  jdomains.SetArray();

  BOOST_FOREACH(const DomainInfo& di, domains) {
    Value jdi;
    jdi.SetObject();
    jdi.AddMember("name", di.zone.c_str(), doc.GetAllocator());
    jdi.AddMember("kind", di.getKindString(), doc.GetAllocator());
    Value masters;
    masters.SetArray();
    BOOST_FOREACH(const string& master, di.masters) {
      Value value(master.c_str(), doc.GetAllocator());
      masters.PushBack(value, doc.GetAllocator());
    }
    jdi.AddMember("masters", masters, doc.GetAllocator());
    jdi.AddMember("serial", di.serial, doc.GetAllocator());
    jdi.AddMember("notified_serial", di.notified_serial, doc.GetAllocator());
    jdi.AddMember("last_check", (unsigned int) di.last_check, doc.GetAllocator());
    jdomains.PushBack(jdi, doc.GetAllocator());
  }
  doc.AddMember("domains", jdomains, doc.GetAllocator());
  resp->body = makeStringFromDocument(doc);
}

void StatWebServer::jsonstat(HttpRequest* req, HttpResponse* resp)
{
  string command;

  if(req->parameters.count("command")) {
    command = req->parameters["command"];
    req->parameters.erase("command");
  }

  if(command=="get") {
    if(req->parameters.empty()) {
      vector<string> entries = S.getEntries();
      BOOST_FOREACH(string& ent, entries) {
        req->parameters[ent];
      }
      req->parameters["version"];
      req->parameters["uptime"];
    }

    string variable, value;
    
    Document doc;
    doc.SetObject();
    for(varmap_t::const_iterator iter = req->parameters.begin(); iter != req->parameters.end() ; ++iter) {
      variable = iter->first;
      if(variable == "version") {
        value = VERSION;
      }
      else if(variable == "uptime") {
        value = lexical_cast<string>(time(0) - s_starttime);
      }
      else 
        value = lexical_cast<string>(S.read(variable));
      Value jval;
      jval.SetString(value.c_str(), value.length(), doc.GetAllocator());
      doc.AddMember(variable.c_str(), jval, doc.GetAllocator());
    }
    resp->body = makeStringFromDocument(doc);
    return;
  }
  else if(command=="config") {
    apiServerConfig(req, resp);
    return;
  }
  else if(command == "flush-cache") {
    extern PacketCache PC;
    int number; 
    if(req->parameters["domain"].empty())
      number = PC.purge();
    else
      number = PC.purge(req->parameters["domain"]);
      
    map<string, string> object;
    object["number"]=lexical_cast<string>(number);
    //cerr<<"Flushed cache for '"<<parameters["domain"]<<"', cleaned "<<number<<" records"<<endl;
    resp->body = returnJSONObject(object);
    return;
  }
  else if(command == "pdns-control") {
    if(req->method!="POST")
      throw HttpMethodNotAllowedException();
    // cout<<"post: "<<post<<endl;
    rapidjson::Document document;
    if(document.Parse<0>(req->body.c_str()).HasParseError()) {
      resp->status = 400;
      resp->body = returnJSONError("Unable to parse JSON");
      return;
    }
    // cout<<"Parameters: '"<<document["parameters"].GetString()<<"'\n";
    vector<string> parameters;
    stringtok(parameters, document["parameters"].GetString(), " \t");
    
    DynListener::g_funk_t* ptr=0;
    if(!parameters.empty())
      ptr = DynListener::getFunc(toUpper(parameters[0]));
    map<string, string> m;
    
    if(ptr) {
      m["result"] = (*ptr)(parameters, 0);
    } else {
      resp->status = 404;
      m["error"]="No such function "+toUpper(parameters[0]);
    }
    resp->body = returnJSONObject(m);
    return;
  }
  else if(command == "zone-rest") { // http://jsonstat?command=zone-rest&rest=/powerdns.nl/www.powerdns.nl/a
    vector<string> parts;
    stringtok(parts, req->parameters["rest"], "/");
    if(parts.size() != 3) {
      resp->status = 400;
      resp->body = returnJSONError("Could not parse rest parameter");
      return;
    }
    UeberBackend B;
    SOAData sd;
    sd.db = (DNSBackend*)-1;
    if(!B.getSOA(parts[0], sd) || !sd.db) {
      resp->status = 404;
      resp->body = returnJSONError("Could not find domain '"+parts[0]+"'");
      return;
    }
    
    QType qtype;
    qtype=parts[2];
    string qname=parts[1];
    extern PacketCache PC;
    PC.purge(qname);
    // cerr<<"domain id: "<<sd.domain_id<<", lookup name: '"<<parts[1]<<"', for type: '"<<qtype.getName()<<"'"<<endl;
    
    if(req->method == "GET") {
      B.lookup(qtype, parts[1], 0, sd.domain_id);
      
      DNSResourceRecord rr;
      string ret = "{ \"records\": [";
      map<string, string> object;
      bool first=1;
      
      while(B.get(rr)) {
        if(!first) ret += ", ";
          first=false;
        object.clear();
        object["name"] = rr.qname;
        object["type"] = rr.qtype.getName();
        object["ttl"] = lexical_cast<string>(rr.ttl);
        object["priority"] = lexical_cast<string>(rr.priority);
        object["content"] = rr.content;
        ret+=returnJSONObject(object);
      }
      ret+="]}";
      resp->body = ret;
      return;
    }
    else if(req->method=="DELETE") {
      sd.db->replaceRRSet(sd.domain_id, qname, qtype, vector<DNSResourceRecord>());
      
    }
    else if(req->method=="POST") {
      rapidjson::Document document;
      if(document.Parse<0>(req->body.c_str()).HasParseError()) {
        resp->status = 400;
        resp->body = returnJSONError("Unable to parse JSON");
        return;
      }
      
      DNSResourceRecord rr;
      vector<DNSResourceRecord> rrset;
      const rapidjson::Value &records= document["records"];
      for(rapidjson::SizeType i = 0; i < records.Size(); ++i) {
        const rapidjson::Value& record = records[i];
        rr.qname=record["name"].GetString();
        rr.content=record["content"].GetString();
        rr.qtype=record["type"].GetString();
        rr.domain_id = sd.domain_id;
        rr.auth=0;
        rr.ttl=intFromJson(record["ttl"]);
        rr.priority=intFromJson(record["priority"]);
        
        rrset.push_back(rr);
        
        if(rr.qtype.getCode() == QType::MX || rr.qtype.getCode() == QType::SRV) 
          rr.content = lexical_cast<string>(rr.priority)+" "+rr.content;
          
        try {
          shared_ptr<DNSRecordContent> drc(DNSRecordContent::mastermake(rr.qtype.getCode(), 1, rr.content));
          string tmp=drc->serialize(rr.qname);
        }
        catch(std::exception& e) 
        {
          resp->body = returnJSONError("Following record had a problem: "+rr.qname+" IN " +rr.qtype.getName()+ " " + rr.content+": "+e.what());
          return;
        }
      }
      // but now what
      sd.db->startTransaction(qname);
      sd.db->replaceRRSet(sd.domain_id, qname, qtype, rrset);
      sd.db->commitTransaction();
      resp->body = req->body;
      return;
    }  
  }
  else if(command == "zone") {
    string zonename = req->parameters["zone"];
    if (zonename.empty()) {
      resp->status = 400;
      resp->body = returnJSONError("Must give zone parameter");
    }

    if(req->method == "GET") {
      // get current zone
      resp->body = getZone(zonename);
      return;
    } else if (req->method == "POST") {
      // create
      resp->body = createOrUpdateZone(zonename, true, req->parameters);
      return;
    } else if (req->method == "PUT") {
      // update or create
      resp->body = createOrUpdateZone(zonename, false, req->parameters);
      return;
    } else if (req->method == "DELETE") {
      // delete
      UeberBackend B;
      DomainInfo di;
      if(!B.getDomainInfo(zonename, di)) {
        resp->body = returnJSONError("Deleting domain '"+zonename+"' failed: domain does not exist");
        return;
      }
      if(!di.backend->deleteDomain(zonename)) {
        resp->body = returnJSONError("Deleting domain '"+zonename+"' failed: backend delete failed/unsupported");
        return;
      }
      map<string, string> success; // empty success object
      resp->body = returnJSONObject(success);
      return;
    } else {
      throw HttpMethodNotAllowedException();
    }
  }
  else if(command=="log-grep") {
    resp->body = makeLogGrepJSON(req->parameters["needle"], ::arg()["experimental-logfile"], " pdns[");
    return;
  }
  else if(command=="domains") {
    apiServerZones(req, resp);
    return;
  }

  resp->body = returnJSONError("No or unknown command given");
  resp->status = 404;
  return;
}

static void apiWrapper(boost::function<void(HttpRequest*,HttpResponse*)> handler, HttpRequest* req, HttpResponse* resp) {
  resp->headers["Access-Control-Allow-Origin"] = "*";
  resp->headers["Content-Type"] = "application/json";

  string callback;

  if(req->parameters.count("callback")) {
    callback=req->parameters["callback"];
    req->parameters.erase("callback");
  }
  
  req->parameters.erase("_"); // jQuery cache buster

  handler(req, resp);

  if(!callback.empty()) {
    resp->body = callback + "(" + resp->body + ");";
  }
}

void StatWebServer::registerApiHandler(const string& url, boost::function<void(HttpRequest*,HttpResponse*)> handler) {
  WebServer::HandlerFunction f = boost::bind(&apiWrapper, handler, _1, _2);
  d_ws->registerHandler(url, f);
}

void StatWebServer::cssfunction(HttpRequest* req, HttpResponse* resp)
{
  resp->headers["Cache-Control"] = "max-age=86400";
  resp->headers["Content-Type"] = "text/css";

  ostringstream ret;
  ret<<"* { box-sizing: border-box; margin: 0; padding: 0; }"<<endl;
  ret<<"body { color: black; background: white; margin-top: 1em; font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif; font-size: 10pt; position: relative; }"<<endl;
  ret<<"a { color: #0959c2; }"<<endl;
  ret<<"a:hover { color: #3B8EC8; }"<<endl;
  ret<<".row { width: 940px; max-width: 100%; min-width: 768px; margin: 0 auto; }"<<endl;
  ret<<".row:before, .row:after { display: table; content:\" \"; }"<<endl;
  ret<<".row:after { clear: both; }"<<endl;
  ret<<".columns { position: relative; min-height: 1px; float: left; }"<<endl;
  ret<<".all { width: 100%; }"<<endl;
  ret<<".headl { width: 60%; }"<<endl;
  ret<<".headr { width: 39.5%; float: right; background-repeat: no-repeat; margin-top: 7px; ";
  ret<<"background-image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAJoAAAAUCAYAAAB1RSS/AAAABHNCSVQICAgIfAhkiAAAAAlwSFlzAAACtgAAArYBAHIqtQAAABl0RVh0U29mdHdhcmUAd3d3Lmlua3NjYXBlLm9yZ5vuPBoAABBTSURBVGiBtVp7cFRVmv9u3763b7/f9It00iFACBohgCEyQYgKI49CLV3cWaoEZBcfo2shu7KOtZbjrqOuVQtVWFuOrPqPRU3NgOIDlkgyJEYJwUAqjzEJedFJupN0p9/v+9o/mtve7r790HF+VbeSPue7555zz+98z4ucOXNmgWVZBH4AK5PJGIPBQBqNxpTNZkthGMZCCUxMTBCDg4PyiYkJWTQaRc1mc7Kuri7a1NQU4ssxDAOffPKJAQCynvnII494ESTddO3aNaXT6SS4TplMRj/44IM+7ndXV5dqfn5ewh9306ZNQZqmobu7W11qri0tLX6tVkv19vYqpqampPw+BEFYtVpNGQwG0mKxpJYsWUIKjTE6OiodGBhQ8NcgkUgYjUZDORyOhM1mSxV6fjAYFF+6dEnLb9NoNOR9990X4H53dHSovV4vzpfZvn27T6FQ0Py2sbExorOzU+N2uwmWZUGv15N33nlnuLGxMZy7byyVQEJ//nd9Yuz/lJR/HBdrHSlJ9baIuuV1L4LJ8/Y49pc/KcJX39WRC4MEgskY3Lourmn5rQdbckfe2ijfOBZo+40xNXtNysR9KLZkdVK+9oBf0fBkCABA3NraamTZwjxSKpXUAw884G1paQkUIty5c+f0Fy5cWMIfx+l0Snt6ejTt7e26AwcOuKxWawoAQCQSQW9vr3pxcTHrJTY3Nwe5Tb18+bJ2bGxMzvWhKMpu27bNj6IoCwDQ1tamd7lcRM79genpaaK1tdVQcDG3sXbt2rBWq6X6+/sV3d3d2mKyy5cvj+7cudO7atWqGL99bGxMWuxZOp0utX37du+9994b5A4Qh2AwiObei6Ioe/fdd4eVSiUNAHD16lX1+Pi4nC+zadOmIJ9oZ8+eNeTu3/T0tLSvr0/V3d0dPXr0qJNrZ+KL6MKpjZWUbyxzQMmFIYJcGCISw5+qjE9+M4UqLJmx/RdeWBK+elKfGTjuR+OhWSxx86JS/9D/zsrufDzMdSXGv5J5/vBYBZuKiLi25HS3LDndLUuMX1IYHjvtynQUQjgcFp89e9b8zjvv2BmGyepjWRbeffdd2/nz55cUIqvT6ZSeOHHC7vf7xVyb3W6P58rNzc1liOfxeLJISNM04na7Me63z+fD+P1SqZQupHn+Wty8eVN+4sSJyv7+fnlp6R/g8/nw06dPW0+ePLmUJEmklDxN08iVK1dU5Y7f0dGhvnjxYkElQVFU1jP9Xz5j4pMsSzYwifvPPWnhfsdHPpdnkYwHlk4ivi9/baFDM2IAACYZEi1++qSVTzI+YkN/VEe++726JNE4TE1Nyc6cOWPkt3322Wf6/v7+ki8nEAhgH3zwQWYhDoejINGSyaQoFAphuf2zs7MSAIBIJIImEgmU32ez2RLlruOngGVZ+Oijj6w+n09cWjobg4ODyg8//NBSWhLgu+++K4toJEkin376qancObBkFIl/f7bo2ImxC0om5kUBACK9pzTFZJlEAI0O/kEJABAf+UJOh115+8VH5MZHGkGimc3mRK66BwBoa2szBAIBMUB6w1tbW415QgUwOjqqGB4elgIA1NTU5BGN02IulwsXOqUul0sCADA/P5+3qIqKip+NaARBMBiGMbnt0Wg0z68qF729vepr164pS8k5nU7ZwsJC0U0DAOjp6VHGYjE0t10kEgmqt5TrOwIYqqRWTbmuSQAASM9fiFKy5Fx/Wnaur7Ss53tC8IQ+/fTTM/F4HH3rrbcc/E1nWRYmJyeJtWvXRr7++mt1rnoGANi6devipk2bgsePH7dHIpGs8Ts7O7W1tbXxqqqqJIZhLN+keDweDADA7XbjuWPebpcAACwsLOT1V1VVFSSayWRKvvLKK5P8tmLBTVNTk//hhx/2vv/++5aBgYEsLeB0OqWF7gMAsFqtiYqKivj169c1ueaytbVVv2HDhnChewHS7/fKlSuqPXv2LBaTyw1gAABqa2sjhw4dck1PT0vOnz9v4O+NWFNdlluBqispAABUYSEp/6TgPmRkVba0rGppybFRpZksaDodDkeioqIiT/M4nU4JAMDIyEiez1JTUxN9/PHHFyoqKpJbtmzx5faPj4/LANKOr9VqzRqbi7D4vhof8/PzOMAPhMyZa948OSAIAjiOs/xLSFvzIZFImO3bt+fNn9OqhaDRaMiDBw/Obd26NY8oTqdTWmhtfPT29paMmkOhUJ6CkEgkjFKppOvq6mIvvviis76+PkNqVF1BiQ21yWJjoiobiRlWpQAACMeWaKk5EMu2RQEAiOr7YyBCi2YliMrN0aI+Wjwez+vn/KOZmZk8lbl69eoI97+QeQwEAhgXFFRVVWX1+/1+nGVZyE1bcPB6vRKWZSE35JdKpbTJZCp4qiiKQmZmZnDuEiKqEITWTtN0SfMDALBjx45FiUSSZ35HRkaKakQAgPn5ecnU1FRRQuv1+rz0Qn9/v+ry5ctqgPTh2rFjR9ZB0e78Hzcgedb2NhDQ7vq9C24fQNXm3/gww8qCxJTX/4OfcGyJAwBgS+pSqo3/XFADo0oLqdn2lkeQaAzDIB0dHWqPx5O3YK1WSzIMA7lmEQDAaDSSQv/zEQwGUQCA6urqLKJRFIV4PB6MH3GqVCqS3z83N4cvLi5mEaVUIOD1evHXX399GXedOnXKWkweIJ3r++abb/IcYqPRWDA3xodUKmWEyMCZ/1IolQvMfXcAabN7+vRp68cff2wS8nElVVvihl99cQtV27PmhapspOHvzzmJ5Tsy6RtELGGX7G+7JV2xIysHiqAYq/rFv3h0e96f57drHnjTo2n57TwiJrIOl6SyOWo6cPmWiNAwgj7am2++6Ugmk4IkrK2tjUWjUVRoMXK5PJOHkclkdJ4AAESjURQAYPny5YKRJ59odXV1EX6ea2ZmRpKbf/s5AwEAgO+//17+8ssv1/j9/jzNt3HjxmC542g0GjI318etXQgoirKcxrx+/brKYDAUJPW6desiFy5ciM/MzORpyM7OTl04HEYPHz7synURiJpfxizPj4+T8/0S0jOEiw2rUrh5TRJE+TRAFWba+KvPZung9Hxy9iohwpUMvnRjQkSo8zQ1ICJQbX7Zp2h8LpCa7ZEwUY8Yt21IiHXLMopCkEyFSFZZWRmz2+0FVSqXUL39v6AM5yTr9XpKrVZnab2RkRFZKpXKPHvlypUxvuM+PT0tCQaDWW+lWCDwUzA3N0cIkay2tjbS0tLiL3ccoYNWzPRWVVXFcBxnAACCwSAmRCIOCILA/v373QqFghLqv3Hjhrq9vb1gioIFBNLFoLI8gbKBILdHRNi8ocvOC6nVavLw4cOzAAAKhYJGEARytRo/5A6Hw4JMk8lkmRNht9vjAwMDmU0dGhril3TAbDanDAZD0u12EwAAw8PDCoZhspZQLBD4KRBa17Zt27wPPfSQVyQqO+0IQumHQloeIB0Jr169Onzjxg01QOHDzqGioiJ55MiRW8ePH68UCg6+/PJLY0tLS4Cv1RJjF2W+z5+2UEFnxiqgKhup2/muW7pyV1YAQEfmUN9n/2SOj57PRN4IirHKphe86q2vLSIozktHMBDq+p0u3PkfRpZKZOYtqWyOavd86BZrlxWOOjMTQVH2jjvuCL/wwgtOvV5PAaQ3QyqV5r20SCSSebmhUEiQaCqVKnNfLkk4QnEwmUyk2WzOaNDp6emsU14qEABIO87Hjh2b5K79+/e7i8kLVS0UCgXF19blINfEAwCoVCpBDcShsbExVKw/FzabLXXs2LFJIT81Go2K+YFPYqpDuvDx7ko+yQAA6NAs5jn9sD1+84KMa2OpJLLw0X2VfJIBALA0iYS6/svoO/ePWcni4KWXjKH2V0x8kgEAJG99Lfd8uLmSSfiFj+j999/v3bt3r/vgwYMzb7zxxthzzz03w9UqOVit1rzFjY6OZiY7NDSUl/4gCIIxmUyZcZYtW1ZQG0mlUloul9Nmszkjn1sCK6cigGEY63A4EtxlsViKOvQOhyOm0WiyyNve3q4vN+IESKeAhKJnISeej/r6+ijfzy2Evr4+Oad19Xo9dejQoVkhbev1ejNE83/xjAXYfPcqDRZ8nz9lhdtjhjr/U0d6RwoGLtH+j7WJyctSAADSM4SHu/9bsFwFAECHXVjwq381ChKtubk50NLSEmhsbAxrNBrBU7hixYq8XMvg4KByamqKmJubw7799ts8H6GqqirGV+XV1dWJQppCq9WSAABWq7WgT/hzBwIAaW3d0NCQpVkCgQDW1dVVVnnI5XLhp06dsuW24zjO1NTUFJ0viqJsfX19Sa3W09Ojfu+996xcCkapVNIoiuaxyGAwkAAAdHBaXIw4AGnNRnqHcQCAxOTlknXdxHirHAAgOXFJBkzxQ5ic6pD/6Nodh9uRT1YxPRaLoW+//XaVWCxmhXyMe+65J8D/jeM4a7FYEkKOL5ceWLp0aUGiVVZWliSax+PBX3rppRp+27PPPjtdLKhpamoKtre3Z53Sr776yrB58+a8LzH4GB4eVr722muCpaaGhoYgQRCFVEoGGzduDF65cqVkqevGjRvqgYEBld1uj8/NzUlIMtsNwnGc4VJMlH+yrNwhFbglxoyrUnTEXVKeDs2K039nSstG5rDyvdscLF26NNnQ0JAX7tM0jQiRzGQyJdevXx/Jba+srBQ0J3q9ngRIBwRisVhQ65UTCNA0jQQCAYx/CZXO+LDb7UmLxZJFYo/Hg1+9erVovTLXtHMgCILevXt30bISh5UrV8ZzTXchUBSFTExMyIQCj7q6ugh3KHDbugSIhN8hHxLb+iQAAGasK+2SmOvTsuY1pWWNqxI/mWgAAI8++uiCTqcrmcTEMIzZt2+fW8hMFvJbuNMoEokEM+FSqZQ2m81/k0+DAADWr1+fZ8IuXrxY8lu3XKAoyu7bt8/NmbFSEDLdPxYSiYTZu3dvJqmKYHJWturhomNKa34ZFskMNACAYt2hQDFZEaGh5XfsDQMAECt2R1Glreja5GsOBP4qoul0Ouro0aO3TCZTQTOkUqnII0eO3FqxYoUgoYRKVQAA/ISl0Ph/60+Dmpqa8syky+Ui+vr6yv4uTavVks8///ytUsV0oWf/GHk+pFIp/cQTT8zqdLos31q36+S8WFcjuE9iTVVK99CpTDQuXbk7qmz8taAGRlAJq9t50o2qllIAACKJitHu+cCF4ApBdS5d/XdB+fqnguLq6upobm4Kx/GyQ3m9Xk+9+uqrk21tbZquri6t1+vFWZYFi8WSdDgcsV27di1qtdqCYb3ZbCZra2sjueaW/yl0XV1dNBwOZ/mT/KIxB6VSSTkcjlhuey44X8lkMqVy5TmC6/V6qrGx0Z8bPY6OjsrWrFkT1el0ec9CUZRVqVSUWq2mqqur4xs2bAgL+XQSiYTJvZcf9Njt9uRdd90Vys2PcQnd5ubmAMMwcPPmTXk0GhUDpCsRVVVVsccee2yBS0PxIZLqacszfZPBP7+qj4+1Kilf+lNuYtkDEU3La3mfcmsfPL4gqfxFrJxPuYll22Kmp/omgpf+zZia7ZEyCT+KGVcn5WsP+uUNh0IAAP8PaQRnE4MgdzkAAAAASUVORK5CYII=);";
  ret<<" width: 154px; height: 20px; }"<<endl;
  ret<<"a#appname { margin: 0; font-size: 27px; color: #666; text-decoration: none; font-weight: bold; display: block; }"<<endl;
  ret<<"footer { border-top:  1px solid #ddd; padding-top: 4px; font-size: 12px; }"<<endl;
  ret<<"footer.row { margin-top: 1em; margin-bottom: 1em; }"<<endl;
  ret<<".panel { background: #f2f2f2; border: 1px solid #e6e6e6; margin: 0 0 22px 0; padding: 20px; }"<<endl;
  ret<<"table.data { width: 100%; border-spacing: 0; border-top: 1px solid #333; }"<<endl;
  ret<<"table.data td { border-bottom: 1px solid #333; padding: 2px; }"<<endl;
  ret<<"table.data tr:nth-child(2n) { background: #e2e2e2; }"<<endl;
  ret<<"table.data tr:hover { background: white; }"<<endl;
  ret<<".ringmeta { margin-bottom: 5px; }"<<endl;
  ret<<".resetring {float: right; }"<<endl;
  ret<<".resetring i { background-image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAoAAAAKCAYAAACNMs+9AAAA/klEQVQY01XPP04UUBgE8N/33vd2XZUWEuzYuMZEG4KFCQn2NhA4AIewAOMBPIG2xhNYeAcKGqkNCdmYlVBZGBIT4FHsbuE0U8xk/kAbqm9TOfI/nicfhmwgDNhvylUT58kxCp4l31L8SfH9IetJ2ev6PwyIwyZWsdb11/gbTK55Co+r8rmJaRPTFJcpZil+pTit7C5awMpA+Zpi1sRFE9MqflYOloYCjY2uP8EdYiGU4CVGUBubxKfOOLjrtOBmzvEilbVb/aQWvhRl0unBZVXe4XdnK+bprwqnhoyTsyZ+JG8Wk0apfExxlcp7PFruXH8gdxamWB4cyW2sIO4BG3czIp78jUIAAAAASUVORK5CYII=); width: 10px; height: 10px; margin-right: 2px; display: inline-block; background-repeat: no-repeat; }"<<endl;
  ret<<".resetring:hover i { background-image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAoAAAAKCAYAAACNMs+9AAAA2ElEQVQY013PMUoDcRDF4c+kEzxCsNNCrBQvIGhnlcYm11EkBxAraw8gglgIoiJpAoKIYlBcgrgopsma3c3fwt1k9cHA480M8xvQp/nMjorOWY5ov7IAYlpjQk7aYxcuWBpwFQgJnUcaYk7GhEDIGL5w+MVpKLIRyR2b4JOjvGhUKzHTv2W7iuSN479Dvu9plf1awbQ6y3x1sU5tjpVJcMbakF6Ycoas8Dl5xEHJ160wRdfqzXfa6XQ4PLDlicWUjxHxZfndL/N+RhiwNzl/Q6PDhn/qsl76H7prcApk2B1aAAAAAElFTkSuQmCC);}"<<endl;
  ret<<".resizering {float: right;}"<<endl;
  resp->body = ret.str();
}

void StatWebServer::launch()
{
  try {
    d_ws->registerHandler("/", boost::bind(&StatWebServer::indexfunction, this, _1, _2));
    d_ws->registerHandler("/style.css", boost::bind(&StatWebServer::cssfunction, this, _1, _2));
    if(::arg().mustDo("experimental-json-interface")) {
      registerApiHandler("/servers/localhost/config", &apiServerConfig);
      registerApiHandler("/servers/localhost/search-log", &apiServerSearchLog);
      registerApiHandler("/servers/localhost/zones", &apiServerZones);
      // legacy dispatch
      registerApiHandler("/jsonstat", boost::bind(&StatWebServer::jsonstat, this, _1, _2));
    }
    d_ws->go();
  }
  catch(...) {
    L<<Logger::Error<<"StatWebserver thread caught an exception, dying"<<endl;
    exit(1);
  }
}

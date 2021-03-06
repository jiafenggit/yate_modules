/**
 * forwarder.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Forwarding on noanswer.
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <yatephone.h>

using namespace TelEngine;
namespace { // anonymous

class ForwardRec : public NamedString
{
public:
    ForwardRec(const char* name, const char* value, const char* forwardTo, const char* delay, RefObject* userData)
        : NamedString(name, value), m_forwardTo(forwardTo), m_delay(delay), m_userData(userData) {
        if (m_userData)
            m_userData->ref();
    }
    virtual ~ForwardRec() {
        if (m_userData)
            m_userData->deref();
    }

    virtual void* getObject(const String& name) const {
        if (name == "ForwardRec")
            return (void*)this;
        return NamedString::getObject(name);
    }

    inline String& getForwardTo() {
        return m_forwardTo;
    }

    inline String& getDelay() {
        return m_delay;
    }

    inline RefObject* getUserData() {
        return m_userData;
    }
        
private:
    String m_forwardTo;
    String m_delay;
    RefObject* m_userData;
};

class ForwarderModule : public Module
{
public:
    enum {
        CallExecute = Private,
        ChanDisconnected = (Private << 1),
        CallAnswered = (Private << 2)
    };
    ForwarderModule();
    ~ForwarderModule();
    bool unload();
    virtual void initialize();
    virtual bool received(Message& msg, int id);
    bool msgExecute(Message& msg);
    bool msgDisconnected(Message& msg);
    bool msgAnswered(Message& msg);
private:
    bool m_init;
    HashList m_hash;
    String m_account;
    String m_get_query;
    int m_disconnected_pri;
    int m_answered_pri;
    int m_execute_pri;
};

// copy parameters from SQL result to a NamedList
static void copyParams(NamedList& lst, Array* a)
{
    if (!a)
        return;
    for (int i = 0; i < a->getColumns(); i++) {
        String* s = YOBJECT(String,a->get(i,0));
        if (!(s && *s))
            continue;
        String name = *s;
        for (int j = 1; j < a->getRows(); j++) {
            s = YOBJECT(String,a->get(i,j));
            if (s)
                lst.setParam(name,*s);
        }
    }
}

INIT_PLUGIN(ForwarderModule);

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow && !__plugin.unload())
        return false;
    return true;
}


bool ForwarderModule::unload()
{
    if (!lock(500000))
        return false;
    uninstallRelays();
    unlock();
    return true;
}

bool ForwarderModule::msgExecute(Message& msg)
{
    Message db("database");
    String query(m_get_query);
    msg.replaceParams(query, true);
    // "SELECT * FROM forwarder WHERE sourceNumber = '";
    //query += msg.getValue("called");
    //query += "'";
    db.addParam("query", query);
    db.addParam("account", m_account);
    if (!Engine::dispatch(db) || db.getIntValue("rows") < 1) {
        const char* error = db.getValue("error","failure");
        Debug(&__plugin, DebugWarn, "Could not fetch db data. Error:  '%s'", error);
        return false;
    }
    Array *result = static_cast<Array*>(db.userObject("Array"));
    if (!result) {
        Debug(&__plugin, DebugWarn, "Result array is NULL");
        return false;
    }
    
    if (result->getRows() <= 1 || result->getColumns() < 3) {
        Debug(&__plugin, DebugInfo, "Result array is empty");
    }

    NamedList lst("templist");
    copyParams(lst, result);
    String dbg;
    lst.dump(dbg, ":", '"', true);

    String delay = result->get(2,1)->toString();
    RefObject* data = msg.userData();
    m_hash.append(new ForwardRec(msg.getValue("id"),
                                 result->get(0, 1)->toString(),
                                 result->get(1, 1)->toString(),
                                 delay,
                                 data));
    Debug(&__plugin, DebugMild, "Added call %s with delay %s. %d calls in list. Result set: %s", msg.getValue("id"), delay.c_str(), m_hash.count(), dbg.c_str());
    msg.setParam("maxcall", delay);
    return false;
}

bool ForwarderModule::msgDisconnected(Message& msg)
{
    Debug(&__plugin, DebugMild, "Processing disconnected %s to %s, reason: %s",
          msg.getValue("id"), msg.getValue("targetid"), msg.getValue("reason"));
    ForwardRec *rec;
    GenObject* obj;
    String id = msg.getValue("targetid");
    obj = m_hash[id];
    if (obj){
        rec = static_cast<ForwardRec*>(obj->getObject("ForwardRec"));
        if (rec) {
            m_hash.remove(rec, true);
            Debug(&__plugin, DebugMild, "Deleted call %s. %d calls remaining", id.c_str(), m_hash.count());
            return false;
        }
    }
    id = msg.getValue("id");
    obj = m_hash[id];
    if (!obj)
        return false;
    rec = static_cast<ForwardRec*>(obj->getObject("ForwardRec"));
    if (!rec)
        return false;
    String reason = static_cast<String>(msg.getParam("reason"));
    if (reason == "noanswer" || reason == "noroute" || reason == "looping") {
        Debug(&__plugin, DebugMild, "Route call to %s", rec->getForwardTo().c_str());
        Message m("call.route");
        m.setParam("caller", (String)rec);
        m.setParam("callername", (String)rec);
        m.setParam("called", rec->getForwardTo());
        if (!Engine::dispatch(m) || (m.retValue() == "-") || (m.retValue() == "error")) {
            Debug(&__plugin,DebugWarn,"Forwarded call from %s to %s routing failed",
                  rec->c_str(), rec->getForwardTo().c_str());
            return false;
        }
        Message exec("call.execute");
        exec.userData(rec->getUserData());
        exec.setParam("id", id);
        exec.setParam("caller", (String)rec);
        exec.setParam("callername", (String)rec);
        exec.setParam("called", rec->getForwardTo());
        exec.setParam("status", "outgoing");
        exec.setParam("callto", m.retValue());
        Engine::dispatch(exec);
    }
    m_hash.remove(rec, true);
    Debug(&__plugin, DebugMild, "Deleted call %s. %d calls remaining", id.c_str(), m_hash.count());
    return false;
}

bool ForwarderModule::msgAnswered(Message &msg)
{
    String id = msg.getValue("targetid");
    GenObject *obj = m_hash[id];
    if (!obj)
        return false;
    ForwardRec *rec = (ForwardRec*)obj->getObject("ForwardRec");
    if (!rec)
        return false;
    m_hash.remove(rec, true);
    Debug(&__plugin, DebugMild, "Deleted call %s. %d calls remaining", id.c_str(), m_hash.count());
    return false;
}

bool ForwarderModule::received(Message& msg, int id)
{
    switch (id) {
        case CallExecute:
            return msgExecute(msg);
        case ChanDisconnected:
            return msgDisconnected(msg);
        case CallAnswered:
            return msgAnswered(msg);
        default:
            return Module::received(msg,id);
    }
}

ForwarderModule::ForwarderModule()
    : Module("forwarder","misc",true),
      m_init(false)
{
    Output("Loaded module Forwarder");
}

ForwarderModule::~ForwarderModule()
{
    Output("Unloading module Forwarder");
}

void ForwarderModule::initialize()
{
    Output("Initializing module Forwarder");
    Configuration cfg(Engine::configFile("forwarder"));
    lock();
    m_account = cfg.getValue("general","account");
    m_get_query = cfg.getValue("general","query");
    m_disconnected_pri =  cfg.getIntValue("priorities","chan.disconnected", 1, 0, 100, true);
    m_execute_pri = cfg.getIntValue("priorities","call.execute", 10, 0, 100, true);
    m_answered_pri = cfg.getIntValue("priorities","call.answered", 10, 0, 100, true);
    unlock();
    if (!m_init && !m_account.null() && !m_get_query.null()) {
        setup();
        installRelay(ChanDisconnected, "chan.disconnected", m_disconnected_pri);
        installRelay(CallExecute, "call.execute", m_execute_pri);
        installRelay(CallAnswered, "call.answered", m_answered_pri);
        m_init = true;
    }
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */

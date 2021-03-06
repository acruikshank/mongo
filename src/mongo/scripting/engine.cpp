// engine.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/scripting/engine.h"

#include <cctype>
#include <boost/filesystem/operations.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/scripting/bench.h"
#include "mongo/util/file.h"
#include "mongo/util/text.h"

namespace mongo {
    long long Scope::_lastVersion = 1;
    static const unsigned kMaxJsFileLength = std::numeric_limits<unsigned>::max() - 1;

    ScriptEngine::ScriptEngine() : _scopeInitCallback() {
    }

    ScriptEngine::~ScriptEngine() {
    }

    Scope::Scope() : _localDBName(""),
                     _loadedVersion(0),
                     _numTimeUsed(0),
                     _lastRetIsNativeCode(false) {
    }

    Scope::~Scope() {
    }

    void Scope::append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName) {
        int t = type(scopeName);
        switch (t) {
        case Object:
            builder.append(fieldName, getObject(scopeName));
            break;
        case Array:
            builder.appendArray(fieldName, getObject(scopeName));
            break;
        case NumberDouble:
            builder.append(fieldName, getNumber(scopeName));
            break;
        case NumberInt:
            builder.append(fieldName, getNumberInt(scopeName));
            break;
        case NumberLong:
            builder.append(fieldName, getNumberLongLong(scopeName));
            break;
        case String:
            builder.append(fieldName, getString(scopeName));
            break;
        case Bool:
            builder.appendBool(fieldName, getBoolean(scopeName));
            break;
        case jstNULL:
        case Undefined:
            builder.appendNull(fieldName);
            break;
        case Date:
            // TODO: make signed
            builder.appendDate(fieldName, Date_t((unsigned long long)getNumber(scopeName)));
            break;
        case Code:
            builder.appendCode(fieldName, getString(scopeName));
            break;
        default:
            uassert(10206,  str::stream() << "can't append type from: " << t, 0);
        }
    }

    int Scope::invoke(const char* code, const BSONObj* args, const BSONObj* recv, int timeoutMs) {
        ScriptingFunction func = createFunction(code);
        uassert(10207,  "compile failed", func);
        return invoke(func, args, recv, timeoutMs);
    }

    bool Scope::execFile(const string& filename, bool printResult, bool reportError,
                         int timeoutMs) {
#ifdef _WIN32
        boost::filesystem::path p(toWideString(filename.c_str()));
#else
        boost::filesystem::path p(filename);
#endif
        if (!exists(p)) {
            log() << "file [" << filename << "] doesn't exist" << endl;
            return false;
        }

        // iterate directories and recurse using all *.js files in the directory
        if (boost::filesystem::is_directory(p)) {
            boost::filesystem::directory_iterator end;
            bool empty = true;

            for (boost::filesystem::directory_iterator it (p); it != end; it++) {
                empty = false;
                boost::filesystem::path sub(*it);
                if (!endsWith(sub.string().c_str(), ".js"))
                    continue;
                if (!execFile(sub.string(), printResult, reportError, timeoutMs))
                    return false;
            }

            if (empty) {
                log() << "directory [" << filename << "] doesn't have any *.js files" << endl;
                return false;
            }

            return true;
        }

        File f;
        f.open(filename.c_str(), true);
        
        if (!f.is_open() || f.bad())
            return false;
        
        fileofs fo = f.len();
        if (fo > kMaxJsFileLength) {
            warning() << "attempted to execute javascript file larger than 2GB" << endl;
            return false;
        }
        unsigned len = static_cast<unsigned>(fo);
        boost::scoped_array<char> data (new char[len+1]);
        data[len] = 0;
        f.read(0, data.get(), len);

        int offset = 0;
        if (data[0] == '#' && data[1] == '!') {
            const char* newline = strchr(data.get(), '\n');
            if (!newline)
                return true; // file of just shebang treated same as empty file
            offset = newline - data.get();
        }

        StringData code(data.get() + offset, len - offset);
        return exec(code, filename, printResult, reportError, timeoutMs);
    }

    void Scope::storedFuncMod() {
        _lastVersion++;
    }

    void Scope::validateObjectIdString(const string& str) {
        uassert(10448, "invalid object id: length", str.size() == 24);
        for (size_t i = 0; i < str.size(); i++)
            uassert(10430,  "invalid object id: not hex", std::isxdigit(str.at(i)));
    }

    void Scope::loadStored(bool ignoreNotConnected) {
        if (_localDBName.size() == 0) {
            if (ignoreNotConnected)
                return;
            uassert(10208,  "need to have locallyConnected already", _localDBName.size());
        }

        if (_loadedVersion == _lastVersion)
            return;

        _loadedVersion = _lastVersion;
        string coll = _localDBName + ".system.js";

        static DBClientBase* db = createDirectClient();
        auto_ptr<DBClientCursor> c = db->query(coll, Query(), 0, 0, NULL, QueryOption_SlaveOk, 0);
        massert(16669, "unable to get db client cursor from query", c.get());

        set<string> thisTime;
        while (c->more()) {
            BSONObj o = c->nextSafe();
            BSONElement n = o["_id"];
            BSONElement v = o["value"];

            uassert(10209, str::stream() << "name has to be a string: " << n, n.type() == String);
            uassert(10210, "value has to be set", v.type() != EOO);

            try {
                setElement(n.valuestr(), v);
                thisTime.insert(n.valuestr());
                _storedNames.insert(n.valuestr());
            }
            catch (const DBException& setElemEx) {
                log() << "unable to load stored JavaScript function " << n.valuestr()
                      << "(): " << setElemEx.what() << endl;
            }
        }

        // remove things from scope that were removed from the system.js collection
        for (set<string>::iterator i = _storedNames.begin(); i != _storedNames.end(); ) {
            if (thisTime.count(*i) == 0) {
                string toDelete = str::stream() << "delete " << *i;
                _storedNames.erase(i++);
                execSetup(toDelete, "clean up scope");
            }
            else {
                ++i;
            }
        }
    }

    ScriptingFunction Scope::createFunction(const char* code) {
        if (code[0] == '/' && code [1] == '*') {
            code += 2;
            while (code[0] && code[1]) {
                if (code[0] == '*' && code[1] == '/') {
                    code += 2;
                    break;
                }
                code++;
            }
        }

        FunctionCacheMap::iterator i = _cachedFunctions.find(code);
        if (i != _cachedFunctions.end())
            return i->second;
        // NB: we calculate the function number for v8 so the cache can be utilized to
        //     lookup the source on an exception, but SpiderMonkey uses the value
        //     returned by JS_CompileFunction.
        ScriptingFunction defaultFunctionNumber = getFunctionCache().size() + 1;
        ScriptingFunction& actualFunctionNumber = _cachedFunctions[code];
        actualFunctionNumber = _createFunction(code, defaultFunctionNumber);
        return actualFunctionNumber;
    }

    namespace JSFiles {
        extern const JSFile collection;
        extern const JSFile db;
        extern const JSFile mongo;
        extern const JSFile mr;
        extern const JSFile query;
        extern const JSFile utils;
        extern const JSFile utils_sh;
    }

    void Scope::execCoreFiles() {
        execSetup(JSFiles::utils);
        execSetup(JSFiles::utils_sh);
        execSetup(JSFiles::db);
        execSetup(JSFiles::mongo);
        execSetup(JSFiles::mr);
        execSetup(JSFiles::query);
        execSetup(JSFiles::collection);
    }

    /** install BenchRunner suite */
    void Scope::installBenchRun() {
        injectNative("benchRun", BenchRunner::benchRunSync);
        injectNative("benchRunSync", BenchRunner::benchRunSync);
        injectNative("benchStart", BenchRunner::benchStart);
        injectNative("benchFinish", BenchRunner::benchFinish);
    }

    typedef map<string, list<Scope*> > PoolToScopes;
    typedef unordered_set<Scope*> ActiveScopes;

    class ScopeCache {
    public:
        ScopeCache() : _mutex("ScopeCache") {
        }

        ~ScopeCache() {
            if (inShutdown())
                return;
            clear();
        }

        void done(const string& pool, Scope* s) {
            scoped_lock lk(_mutex);
            list<Scope*>& l = _pools[pool];
            bool oom = s->hasOutOfMemoryException();

            const unsigned kMaxPoolSize = 10;
            const int kMaxScopeReuse = 10;

            // 'orphaned' scopes were in-use while the ScopeCache was clear()ed.  they should not
            // be returned to any pool since authentication credentials may have changed.
            bool orphaned = !_active.erase(s);

            // do not keep too many contexts, use them for too long, or reuse after error
            if (l.size() > kMaxPoolSize ||
                s->getTimeUsed() > kMaxScopeReuse ||
                !s->getError().empty() ||
                oom ||
                orphaned) {
                delete s;
            }
            else {
                l.push_back(s);
                s->reset();
            }

            if (oom) {
                // out of mem, make some room
                log() << "Clearing all idle JS contexts due to out of memory" << endl;
                clear();
            }
        }

        Scope* get(const string& pool) {
            scoped_lock lk(_mutex);
            list<Scope*>& l = _pools[pool];
            if (l.size() == 0)
                return NULL;

            Scope* s = l.back();
            l.pop_back();
            _active.insert(s);
            s->reset();
            s->incTimeUsed();
            return s;
        }

        void clear() {
            set<Scope*> seen;
            for (PoolToScopes::iterator i = _pools.begin(); i != _pools.end(); ++i) {
                for (list<Scope*>::iterator j = i->second.begin(); j != i->second.end(); ++j) {
                    Scope* s = *j;
                    fassert(16652, seen.insert(s).second);
                    delete s;
                }
            }
            _pools.clear();
            _active.clear();

        }

        /**
         * Add a scope to the active (or in-use) set of scopes.
         * A scope is considered active if it is not in the pool.  If the pool is cleared while an
         * active scope is still running, the active scope will become orphaned.  When the orphaned
         * scope calls done(), it will be freed instead of being placed back in the pool for reuse.
         *
         * This should only be called when ScriptEngine::getPooledScope() must create a new scope
         * for the pool.
         */
        void addActive(Scope* scope) {
            _active.insert(scope);
        }

    private:
        PoolToScopes _pools;    // protected by _mutex
        ActiveScopes _active;   // protected by _mutex
        mongo::mutex _mutex;
    };

    thread_specific_ptr<ScopeCache> scopeCache;

    class PooledScope : public Scope {
    public:
        PooledScope(const std::string& pool, Scope* real) : _pool(pool), _real(real) {
            _real->loadStored(true);
        };
        virtual ~PooledScope() {
            ScopeCache* sc = scopeCache.get();
            if (sc) {
                sc->done(_pool, _real);
                _real = NULL;
            }
            else {
                // this means that the Scope was killed from a different thread
                // for example a cursor got timed out that has a $where clause
                LOG(3) << "warning: scopeCache is empty!" << endl;
                delete _real;
                _real = 0;
            }
        }

        // wrappers for the derived (_real) scope
        void reset() { _real->reset(); }
        void init(const BSONObj* data) { _real->init(data); }
        void localConnect(const char* dbName) { _real->localConnect(dbName); }
        void setLocalDB(const string& dbName) { _real->setLocalDB(dbName); }
        void loadStored(bool ignoreNotConnected = false) { _real->loadStored(ignoreNotConnected); }
        void externalSetup() { _real->externalSetup(); }
        void gc() { _real->gc(); }
        bool isKillPending() const { return _real->isKillPending(); }
        int type(const char* field) { return _real->type(field); }
        string getError() { return _real->getError(); }
        bool hasOutOfMemoryException() { return _real->hasOutOfMemoryException(); }
        void rename(const char* from, const char* to) { _real->rename(from, to); }
        double getNumber(const char* field) { return _real->getNumber(field); }
        string getString(const char* field) { return _real->getString(field); }
        bool getBoolean(const char* field) { return _real->getBoolean(field); }
        BSONObj getObject(const char* field) { return _real->getObject(field); }
        void setNumber(const char* field, double val) { _real->setNumber(field, val); }
        void setString(const char* field, const StringData& val) { _real->setString(field, val); }
        void setElement(const char* field, const BSONElement& val) {
            _real->setElement(field, val);
        }
        void setObject(const char* field, const BSONObj& obj, bool readOnly = true) {
            _real->setObject(field, obj, readOnly);
        }
        bool isLastRetNativeCode() { return _real->isLastRetNativeCode(); }

        void setBoolean(const char* field, bool val) { _real->setBoolean(field, val); }
        void setFunction(const char* field, const char* code) { _real->setFunction(field, code); }
        ScriptingFunction createFunction(const char* code) { return _real->createFunction(code); }
        int invoke(ScriptingFunction func, const BSONObj* args, const BSONObj* recv,
                   int timeoutMs, bool ignoreReturn, bool readOnlyArgs, bool readOnlyRecv) {
            return _real->invoke(func, args, recv, timeoutMs, ignoreReturn,
                                 readOnlyArgs, readOnlyRecv);
        }
        bool exec(const StringData& code, const string& name, bool printResult, bool reportError,
                  bool assertOnError, int timeoutMs = 0) {
            return _real->exec(code, name, printResult, reportError, assertOnError, timeoutMs);
        }
        bool execFile(const string& filename, bool printResult, bool reportError,
                      int timeoutMs = 0) {
            return _real->execFile(filename, printResult, reportError, timeoutMs);
        }
        void injectNative(const char* field, NativeFunction func, void* data) {
            _real->injectNative(field, func, data);
        }
        void append(BSONObjBuilder& builder, const char* fieldName, const char* scopeName) {
            _real->append(builder, fieldName, scopeName);
        }

    protected:
        FunctionCacheMap& getFunctionCache() { return _real->getFunctionCache(); }

        ScriptingFunction _createFunction(const char* code, ScriptingFunction functionNumber = 0) {
            return _real->_createFunction(code, functionNumber);
        }

    private:
        string _pool;
        Scope* _real;
    };

    /** Get a scope from the pool of scopes matching the supplied pool name */
    auto_ptr<Scope> ScriptEngine::getPooledScope(const string& pool, const string& scopeType) {
        if (!scopeCache.get())
            scopeCache.reset(new ScopeCache());

        Scope* s = scopeCache->get(pool + scopeType);
        if (!s) {
            s = newScope();
            scopeCache->addActive(s);
        }

        auto_ptr<Scope> p;
        p.reset(new PooledScope(pool + scopeType, s));
        p->setLocalDB(pool);
        p->loadStored(true);
        return p;
    }

    void ScriptEngine::threadDone() {
        ScopeCache* sc = scopeCache.get();
        if (sc)
            sc->clear();
    }

    void (*ScriptEngine::_connectCallback)(DBClientWithCommands&) = 0;
    const char* (*ScriptEngine::_checkInterruptCallback)() = 0;
    unsigned (*ScriptEngine::_getCurrentOpIdCallback)() = 0;
    ScriptEngine* globalScriptEngine = 0;

    bool hasJSReturn(const string& code) {
        size_t x = code.find("return");
        if (x == string::npos)
            return false;

        int quoteCount = 0;
        int singleQuoteCount = 0;
        for (size_t i = 0; i < x; i++) {
            if (code[i] == '"') {
                quoteCount++;
            } else if(code[i] == '\'') {
                singleQuoteCount++;
            }
        }
        // if we are in either single quotes or double quotes return false
        if (quoteCount % 2 != 0 || singleQuoteCount % 2 != 0) {
            return false;
        }

        // return is at start OR preceded by space
        // AND return is not followed by digit or letter
        return (x == 0 || isspace(code[x-1])) &&
               !(isalpha(code[x+6]) || isdigit(code[x+6]));
    }

    const char* jsSkipWhiteSpace(const char* raw) {
        while (raw[0]) {
            while (isspace(*raw)) {
                ++raw;
            }
            if (raw[0] != '/' || raw[1] != '/')
                break;
            while (raw[0] && raw[0] != '\n')
                raw++;
        }
        return raw;
    }
}

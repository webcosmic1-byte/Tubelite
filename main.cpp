#include "httplib.h"
#include <sqlite3.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return v && *v ? std::string(v) : fallback;
}

static std::string sha256(const std::string& input) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    std::ostringstream out;
    for (unsigned char c : hash) out << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return out.str();
}

static std::string random_token(size_t n = 48) {
    static const char chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<size_t> d(0, sizeof(chars) - 2);
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s += chars[d(gen)];
    return s;
}

static std::string html_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += c;
        }
    }
    return r;
}

static std::string json_escape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (c < 32) {
                    std::ostringstream x;
                    x << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                    r += x.str();
                } else r += char(c);
        }
    }
    return r;
}

static std::string cookie_value(const httplib::Request& req, const std::string& name) {
    if (!req.has_header("Cookie")) return "";
    std::string cookies = req.get_header_value("Cookie");
    std::string prefix = name + "=";
    size_t pos = 0;
    while (pos < cookies.size()) {
        while (pos < cookies.size() && (cookies[pos] == ' ' || cookies[pos] == ';')) ++pos;
        if (cookies.compare(pos, prefix.size(), prefix) == 0) {
            size_t end = cookies.find(';', pos);
            return cookies.substr(pos + prefix.size(),
                                  end == std::string::npos ? std::string::npos : end - pos - prefix.size());
        }
        size_t next = cookies.find(';', pos);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return "";
}

struct User {
    int id = 0;
    std::string username;
};

class DB {
    sqlite3* db_ = nullptr;
    std::mutex mu_;

    bool exec_locked(const std::string& sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cerr << "SQLite error: " << (err ? err : "unknown") << "\n";
            sqlite3_free(err);
            return false;
        }
        return true;
    }

public:
    explicit DB(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
            throw std::runtime_error("Cannot open database");
        exec_locked("PRAGMA journal_mode=WAL;");
        exec_locked("PRAGMA foreign_keys=ON;");
        exec_locked(
            "CREATE TABLE IF NOT EXISTS users("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "username TEXT UNIQUE NOT NULL,"
            "password_hash TEXT NOT NULL,"
            "created_at INTEGER NOT NULL);");

        exec_locked(
            "CREATE TABLE IF NOT EXISTS videos("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL,"
            "title TEXT NOT NULL,"
            "description TEXT DEFAULT '',"
            "filename TEXT NOT NULL,"
            "thumb TEXT DEFAULT '',"
            "views INTEGER DEFAULT 0,"
            "created_at INTEGER NOT NULL,"
            "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE);");

        exec_locked(
            "CREATE TABLE IF NOT EXISTS likes("
            "user_id INTEGER NOT NULL,"
            "video_id INTEGER NOT NULL,"
            "PRIMARY KEY(user_id,video_id),"
            "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
            "FOREIGN KEY(video_id) REFERENCES videos(id) ON DELETE CASCADE);");

        exec_locked(
            "CREATE TABLE IF NOT EXISTS comments("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_id INTEGER NOT NULL,"
            "video_id INTEGER NOT NULL,"
            "body TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,"
            "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE,"
            "FOREIGN KEY(video_id) REFERENCES videos(id) ON DELETE CASCADE);");
    }

    ~DB() { if (db_) sqlite3_close(db_); }

    bool create_user(const std::string& username, const std::string& hash, int& id) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO users(username,password_hash,created_at) VALUES(?,?,?)",
            -1, &st, nullptr);
        sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, std::time(nullptr));
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (ok) id = (int)sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(st);
        return ok;
    }

    bool login(const std::string& username, const std::string& hash, User& u) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT id,username FROM users WHERE username=? AND password_hash=?",
            -1, &st, nullptr);
        sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(st) == SQLITE_ROW;
        if (ok) {
            u.id = sqlite3_column_int(st, 0);
            u.username = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        }
        sqlite3_finalize(st);
        return ok;
    }

    std::string username(int id) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_, "SELECT username FROM users WHERE id=?", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        std::string s;
        if (sqlite3_step(st) == SQLITE_ROW)
            s = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        return s;
    }

    int add_video(int uid, const std::string& title, const std::string& desc,
                  const std::string& filename, const std::string& thumb) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO videos(user_id,title,description,filename,thumb,created_at) "
            "VALUES(?,?,?,?,?,?)", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, uid);
        sqlite3_bind_text(st, 2, title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, thumb.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, std::time(nullptr));
        int id = -1;
        if (sqlite3_step(st) == SQLITE_DONE) id = (int)sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(st);
        return id;
    }

    void add_view(int vid) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_, "UPDATE videos SET views=views+1 WHERE id=?", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, vid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    bool get_video(int id, std::string& title, std::string& desc, std::string& filename,
                   std::string& thumb, std::string& creator, int& views, int& owner) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT v.title,v.description,v.filename,v.thumb,v.views,v.user_id,u.username "
            "FROM videos v JOIN users u ON u.id=v.user_id WHERE v.id=?",
            -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        bool ok = sqlite3_step(st) == SQLITE_ROW;
        if (ok) {
            title = reinterpret_cast<const char*>(sqlite3_column_text(st,0));
            desc = reinterpret_cast<const char*>(sqlite3_column_text(st,1));
            filename = reinterpret_cast<const char*>(sqlite3_column_text(st,2));
            thumb = reinterpret_cast<const char*>(sqlite3_column_text(st,3));
            views = sqlite3_column_int(st,4);
            owner = sqlite3_column_int(st,5);
            creator = reinterpret_cast<const char*>(sqlite3_column_text(st,6));
        }
        sqlite3_finalize(st);
        return ok;
    }

    std::string videos_json(const std::string& q, int uid = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        std::string sql =
            "SELECT v.id,v.title,v.description,v.thumb,v.views,u.username,"
            "(SELECT COUNT(*) FROM likes l WHERE l.video_id=v.id) "
            "FROM videos v JOIN users u ON u.id=v.user_id ";
        if (!q.empty()) sql += "WHERE v.title LIKE ? OR v.description LIKE ? ";
        sql += "ORDER BY v.created_at DESC LIMIT 60";

        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr);
        std::string like = "%" + q + "%";
        if (!q.empty()) {
            sqlite3_bind_text(st,1,like.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,like.c_str(),-1,SQLITE_TRANSIENT);
        }
        std::string out = "[";
        bool first = true;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (!first) out += ",";
            first = false;
            out += "{\"id\":" + std::to_string(sqlite3_column_int(st,0));
            out += ",\"title\":\"" + json_escape((const char*)sqlite3_column_text(st,1)) + "\"";
            out += ",\"description\":\"" + json_escape((const char*)sqlite3_column_text(st,2)) + "\"";
            out += ",\"thumb\":\"" + json_escape((const char*)sqlite3_column_text(st,3)) + "\"";
            out += ",\"views\":" + std::to_string(sqlite3_column_int(st,4));
            out += ",\"creator\":\"" + json_escape((const char*)sqlite3_column_text(st,5)) + "\"";
            out += ",\"likes\":" + std::to_string(sqlite3_column_int(st,6)) + "}";
        }
        sqlite3_finalize(st);
        out += "]";
        (void)uid;
        return out;
    }

    std::string comments_json(int vid) {
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db_,
            "SELECT c.body,u.username,c.created_at FROM comments c "
            "JOIN users u ON u.id=c.user_id WHERE c.video_id=? ORDER BY c.created_at DESC LIMIT 100",
            -1,&st,nullptr);
        sqlite3_bind_int(st,1,vid);
        std::string out="[";
        bool first=true;
        while(sqlite3_step(st)==SQLITE_ROW){
            if(!first) out+=",";
            first=false;
            out += "{\"body\":\""+json_escape((const char*)sqlite3_column_text(st,0))+"\"";
            out += ",\"username\":\""+json_escape((const char*)sqlite3_column_text(st,1))+"\"}";
        }
        sqlite3_finalize(st);
        return out+"]";
    }

    void comment(int uid,int vid,const std::string& body){
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st=nullptr;
        sqlite3_prepare_v2(db_,
            "INSERT INTO comments(user_id,video_id,body,created_at) VALUES(?,?,?,?)",
            -1,&st,nullptr);
        sqlite3_bind_int(st,1,uid);
        sqlite3_bind_int(st,2,vid);
        sqlite3_bind_text(st,3,body.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_int64(st,4,std::time(nullptr));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    bool toggle_like(int uid,int vid){
        std::lock_guard<std::mutex> lock(mu_);
        sqlite3_stmt* st=nullptr;
        sqlite3_prepare_v2(db_,"SELECT 1 FROM likes WHERE user_id=? AND video_id=?",-1,&st,nullptr);
        sqlite3_bind_int(st,1,uid); sqlite3_bind_int(st,2,vid);
        bool exists=sqlite3_step(st)==SQLITE_ROW;
        sqlite3_finalize(st);
        if(exists){
            sqlite3_prepare_v2(db_,"DELETE FROM likes WHERE user_id=? AND video_id=?",-1,&st,nullptr);
        }else{
            sqlite3_prepare_v2(db_,"INSERT INTO likes(user_id,video_id) VALUES(?,?)",-1,&st,nullptr);
        }
        sqlite3_bind_int(st,1,uid); sqlite3_bind_int(st,2,vid);
        sqlite3_step(st); sqlite3_finalize(st);
        return !exists;
    }
};

class Sessions {
    std::unordered_map<std::string, User> map_;
    std::mutex mu_;
public:
    std::string create(const User& u) {
        std::string token=random_token();
        std::lock_guard<std::mutex> lock(mu_);
        map_[token]=u;
        return token;
    }
    bool get(const std::string& token, User& u) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it=map_.find(token);
        if(it==map_.end()) return false;
        u=it->second; return true;
    }
    void erase(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        map_.erase(token);
    }
};

static bool logged_user(const httplib::Request& req, Sessions& sessions, User& u) {
    std::string token=cookie_value(req,"tl_session");
    return !token.empty() && sessions.get(token,u);
}

static void json_response(httplib::Response& res, const std::string& body, int status=200) {
    res.status=status;
    res.set_content(body,"application/json; charset=UTF-8");
}

int main() {
    std::string data_dir=env_or("DATA_DIR","./data");
    fs::create_directories(data_dir+"/videos");
    DB db(data_dir+"/tubelite.sqlite3");
    Sessions sessions;

    httplib::Server svr;

    svr.set_payload_max_length(512ULL*1024*1024);

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res){
        json_response(res,"{\"ok\":true,\"service\":\"tubelite\"}");
    });

    svr.Get("/api/me", [&](const httplib::Request& req, httplib::Response& res){
        User u;
        if(logged_user(req,sessions,u))
            json_response(res,"{\"loggedIn\":true,\"id\":"+std::to_string(u.id)+",\"username\":\""+json_escape(u.username)+"\"}");
        else json_response(res,"{\"loggedIn\":false}");
    });

    svr.Post("/api/register", [&](const httplib::Request& req, httplib::Response& res){
        auto username=req.get_param_value("username");
        auto password=req.get_param_value("password");
        if(username.size()<3 || username.size()>24 || password.size()<6) {
            json_response(res,"{\"error\":\"Username must be 3-24 chars and password at least 6 chars.\"}",400);
            return;
        }
        if(username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos){
            json_response(res,"{\"error\":\"Username may contain letters, numbers, _ and -.\"}",400);
            return;
        }
        int id=0;
        if(!db.create_user(username,sha256("tubelite:"+password),id)){
            json_response(res,"{\"error\":\"Username already exists.\"}",409);
            return;
        }
        User u{id,username};
        std::string token=sessions.create(u);
        res.set_header("Set-Cookie", "tl_session="+token+"; Path=/; HttpOnly; SameSite=Lax");
        json_response(res,"{\"ok\":true,\"username\":\""+json_escape(username)+"\"}");
    });

    svr.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res){
        auto username=req.get_param_value("username");
        auto password=req.get_param_value("password");
        User u;
        if(!db.login(username,sha256("tubelite:"+password),u)){
            json_response(res,"{\"error\":\"Invalid username or password.\"}",401);
            return;
        }
        std::string token=sessions.create(u);
        res.set_header("Set-Cookie", "tl_session="+token+"; Path=/; HttpOnly; SameSite=Lax");
        json_response(res,"{\"ok\":true,\"username\":\""+json_escape(u.username)+"\"}");
    });

    svr.Post("/api/logout", [&](const httplib::Request& req, httplib::Response& res){
        std::string token=cookie_value(req,"tl_session");
        sessions.erase(token);
        res.set_header("Set-Cookie","tl_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
        json_response(res,"{\"ok\":true}");
    });

    svr.Get("/api/videos", [&](const httplib::Request& req, httplib::Response& res){
        std::string q=req.has_param("q") ? req.get_param_value("q") : "";
        json_response(res,db.videos_json(q));
    });

    svr.Get(R"(/api/video/(\d+))", [&](const httplib::Request& req, httplib::Response& res){
        int id=std::stoi(req.matches[1]);
        std::string title,desc,filename,thumb,creator; int views=0,owner=0;
        if(!db.get_video(id,title,desc,filename,thumb,creator,views,owner)){
            json_response(res,"{\"error\":\"Video not found\"}",404); return;
        }
        db.add_view(id);
        views++;
        std::string out="{\"id\":"+std::to_string(id)+",\"title\":\""+json_escape(title)+
            "\",\"description\":\""+json_escape(desc)+"\",\"filename\":\""+json_escape(filename)+
            "\",\"thumb\":\""+json_escape(thumb)+"\",\"creator\":\""+json_escape(creator)+
            "\",\"owner\":"+std::to_string(owner)+",\"views\":"+std::to_string(views)+"}";
        json_response(res,out);
    });

    svr.Get(R"(/api/video/(\d+)/comments)", [&](const httplib::Request& req, httplib::Response& res){
        json_response(res,db.comments_json(std::stoi(req.matches[1])));
    });

    svr.Post(R"(/api/video/(\d+)/like)", [&](const httplib::Request& req, httplib::Response& res){
        User u; if(!logged_user(req,sessions,u)){ json_response(res,"{\"error\":\"Login required\"}",401); return; }
        bool liked=db.toggle_like(u.id,std::stoi(req.matches[1]));
        json_response(res,std::string("{\"liked\":")+(liked?"true}":"false}"));
    });

    svr.Post(R"(/api/video/(\d+)/comments)", [&](const httplib::Request& req, httplib::Response& res){
        User u; if(!logged_user(req,sessions,u)){ json_response(res,"{\"error\":\"Login required\"}",401); return; }
        std::string body=req.get_param_value("body");
        if(body.empty() || body.size()>1000){ json_response(res,"{\"error\":\"Comment must be 1-1000 characters.\"}",400); return; }
        db.comment(u.id,std::stoi(req.matches[1]),body);
        json_response(res,"{\"ok\":true}");
    });

    svr.Post("/api/upload", [&](const httplib::Request& req, httplib::Response& res){
        User u; if(!logged_user(req,sessions,u)){ json_response(res,"{\"error\":\"Login required\"}",401); return; }
        if(!req.has_file("video")){ json_response(res,"{\"error\":\"Choose a video file.\"}",400); return; }

        const auto& f=req.get_file_value("video");
        const std::string allowed_prefix="video/";
        if(f.content_type.rfind(allowed_prefix,0)!=0){
            json_response(res,"{\"error\":\"Only video MIME types are accepted.\"}",400); return;
        }
        if(f.content.size()>512ULL*1024*1024){
            json_response(res,"{\"error\":\"Maximum upload size is 512 MB.\"}",413); return;
        }

        std::string title=req.has_param("title")?req.get_param_value("title"):"Untitled video";
        std::string desc=req.has_param("description")?req.get_param_value("description"):"";
        std::string thumb=req.has_param("thumb")?req.get_param_value("thumb"):"";
        if(title.empty() || title.size()>150){ json_response(res,"{\"error\":\"Title must be 1-150 characters.\"}",400); return; }

        fs::path ext=fs::path(f.filename).extension();
        std::string safe=std::to_string(std::time(nullptr))+"_"+random_token(10)+ext.string();
        fs::path target=fs::path(data_dir)/"videos"/safe;
        {
            std::ofstream out(target,std::ios::binary);
            out.write(f.content.data(),(std::streamsize)f.content.size());
        }
        int id=db.add_video(u.id,title,desc,safe,thumb);
        if(id<0){
            fs::remove(target);
            json_response(res,"{\"error\":\"Could not save metadata.\"}",500); return;
        }
        json_response(res,"{\"ok\":true,\"id\":"+std::to_string(id)+"}");
    });

    svr.Get(R"(/media/(.+))", [&](const httplib::Request& req, httplib::Response& res){
        std::string name=req.matches[1];
        if(name.find("..")!=std::string::npos || name.find('/')!=std::string::npos || name.find('\\')!=std::string::npos){
            res.status=400; return;
        }
        fs::path p=fs::path(data_dir)/"videos"/name;
        if(!fs::exists(p)){ res.status=404; return; }
        auto file=std::make_shared<std::ifstream>(p,std::ios::binary);
        if(!*file){res.status=404; return;}
        file->seekg(0,std::ios::end); auto size=file->tellg(); file->seekg(0);
        std::string mime="video/mp4";
        auto ext=p.extension().string();
        if(ext==".webm") mime="video/webm";
        else if(ext==".ogg"||ext==".ogv") mime="video/ogg";
        else if(ext==".mov") mime="video/quicktime";
        res.set_header("Accept-Ranges","bytes");
        res.set_content_provider((size_t)size,mime,
            [file](size_t offset,size_t length,httplib::DataSink& sink){
                file->seekg((std::streamoff)offset);
                std::string buf(1024*1024,'\0');
                size_t left=length;
                while(left){
                    size_t n=std::min(left,buf.size());
                    file->read(buf.data(),(std::streamsize)n);
                    std::streamsize got=file->gcount();
                    if(got<=0) break;
                    sink.write(buf.data(),(size_t)got);
                    left-= (size_t)got;
                }
                sink.done();
                return true;
            });
    });

    // Serve frontend.
    svr.set_mount_point("/", "./public");

    int port=std::stoi(env_or("PORT","10000"));
    std::cout<<"TubeLite listening on port "<<port<<"\n";
    if(!svr.listen("0.0.0.0",port)){
        std::cerr<<"Failed to listen\n";
        return 1;
    }
    return 0;
}

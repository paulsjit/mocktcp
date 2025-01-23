#include <memory>
#include <stdexcept>
#include <atomic>
#include <iostream>
#include <map>
#include <stdio.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "mocktcp_lib.h"

namespace py = pybind11;

struct Format {
	char *buf;

	Format(const char *fmt, ...) {
		va_list ap;
		va_start(ap, fmt);
		int len = vsnprintf(NULL, 0, fmt, ap);
		va_end(ap);

		buf = (char *)malloc(len + 1);
		if(!buf)
			throw std::bad_alloc();

		va_start(ap, fmt);
		vsnprintf(buf, len + 1, fmt, ap);
		va_end(ap);
	}

	~Format() {
		free(buf);
	}
};

class req_wrapper {
public:
    ~req_wrapper() { mtcp_free_request(r); }
    req_wrapper(req_t *_r) : r(_r) {}
    req_wrapper(req_wrapper&& obj) { r = obj.r; obj.r = NULL; }
    req_t *r;
};
class c_buf_deleter {
public:
    c_buf_deleter(): m_buf(NULL) {}
    c_buf_deleter(c_buf_deleter&& obj) { m_buf = obj.m_buf; obj.m_buf = NULL; }
    ~c_buf_deleter() {free(m_buf);}
    uint8_t *m_buf;
};

class mtcp_wrapper {
public:
    int rfd;
    int wfd;
    bool started;
    std::map<int, std::unique_ptr<req_wrapper>>eventmap;

    static std::atomic_int refcount;

    mtcp_wrapper(py::object& ro, py::object& wo);
    ~mtcp_wrapper();
    std::string read(uint32_t id);
    void write(uint32_t id, uint8_t *buffer, uint32_t sz);
    int queue_read(uint32_t id);
    int queue_write(uint32_t id, uint8_t *buffer, uint32_t sz);
    std::unique_ptr<req_wrapper> wait_event_one(std::vector<int> wait_ids);

    void get_ref_start_work();
};

std::atomic_int mtcp_wrapper::refcount = 0;

void mtcp_wrapper::get_ref_start_work() {
    int v = 0;
    if(!started) {
        if(mtcp_wrapper::refcount.compare_exchange_weak(v, 1)) {
            mtcp_work_start(rfd, wfd, NULL);
            started = true;
        } else if(v == 1) {
            throw std::runtime_error(std::string(Format("%s : multiple concurrent instances of mocktcp not allowed", __PRETTY_FUNCTION__).buf));
        } else {
            throw std::runtime_error(std::string(Format("%s: extremely strange! refcount == %d in %s", __PRETTY_FUNCTION__, v).buf));
        }
    }
}

mtcp_wrapper::mtcp_wrapper(py::object& ro, py::object& wo) {
    rfd = ro.attr("fileno")().cast<int>();
    wfd = wo.attr("fileno")().cast<int>();
    started = false;
}

mtcp_wrapper::~mtcp_wrapper() {
    int v = 1;
    if(started) {
        mtcp_work_finish();
        if(!mtcp_wrapper::refcount.compare_exchange_weak(v, 0)) {
            throw std::runtime_error(std::string(Format("%s: extremely strange! refcount == %d in %s", __PRETTY_FUNCTION__, v).buf));
        }
    }
}

std::string mtcp_wrapper::read(uint32_t id) {
    get_ref_start_work();

    c_buf_deleter buf;
    uint32_t sz = 0;
    auto r = std::make_unique<req_wrapper>(mtcp_submit_request(id, REQ_TYPE_READ, NULL, 0, true));
    mtcp_wait_for_read_completion(r->r, &buf.m_buf, &sz);
    return std::string((const char *)buf.m_buf, sz);
}

void mtcp_wrapper::write(uint32_t id, uint8_t *buffer, uint32_t sz) {
    get_ref_start_work();

    auto r = std::make_unique<req_wrapper>(mtcp_submit_request(id, REQ_TYPE_WRITE, buffer, sz, true));
    if(mtcp_wait_for_write_completion(r->r))
		throw std::runtime_error(std::string(Format("%s: device denied to accept data, trying %d, expecting %d", __PRETTY_FUNCTION__, sz, mtcp_req_get_error_info(r->r)).buf));
}

int mtcp_wrapper::queue_read(uint32_t id) {
    get_ref_start_work();

    auto r = std::make_unique<req_wrapper>(mtcp_submit_request(id, REQ_TYPE_READ, NULL, 0, false));
    int wid = mtcp_req_get_wait_id(r->r);
    auto ret = eventmap.insert({wid, std::move(r)}); 
    if(!ret.second)
        throw std::runtime_error(std::string(Format("%s: error inserting wid = %d in eventmap", __PRETTY_FUNCTION__, wid).buf));
    return ret.first->first;
}

int mtcp_wrapper::queue_write(uint32_t id, uint8_t *buffer, uint32_t sz) {
    get_ref_start_work();

    auto r = std::make_unique<req_wrapper>(mtcp_submit_request(id, REQ_TYPE_WRITE, buffer, sz, false));
    int wid = mtcp_req_get_wait_id(r->r);
    auto ret = eventmap.insert({wid, std::move(r)});
    if(!ret.second)
        throw std::runtime_error(std::string(Format("%s: error inserting wid = %d in eventmap", __PRETTY_FUNCTION__, wid).buf));
    return ret.first->first;
}

std::unique_ptr<req_wrapper> mtcp_wrapper::wait_event_one(std::vector<int> wait_ids) {
    std::for_each(wait_ids.begin(), wait_ids.end(), [this](int &n){ if(eventmap.find(n) == eventmap.end()) throw std::runtime_error(std::string(Format("%s: event ids %s is not registered", __PRETTY_FUNCTION__, n).buf)); });

    auto eid = mtcp_wait_for_any_completion(wait_ids.data(), wait_ids.size(), NULL);
    if(eid < 0)
        return nullptr;
    return std::move(mtcp_wrapper::eventmap.extract(eid).mapped());
}

PYBIND11_MODULE(mocktcp_lib, m) {
    py::class_<mtcp_wrapper> mocktcp_lib(m, "mocktcp_lib");
    mocktcp_lib.def(py::init<py::object&, py::object&>(), py::keep_alive<1, 2>(), py::keep_alive<1, 3>());
    
    mocktcp_lib.def("read", [](mtcp_wrapper &s, uint32_t id) {
        return py::bytes(s.read(id));
    });
    mocktcp_lib.def("write", [](mtcp_wrapper &s, uint32_t id, std::string buffer) {
        s.write(id, (uint8_t *)buffer.data(), buffer.size());
    });
    
    mocktcp_lib.def("queue_read", [](mtcp_wrapper &s, uint32_t id) {
        return s.queue_read(id);
    });
    mocktcp_lib.def("queue_write", [](mtcp_wrapper &s, uint32_t id, const std::string& buffer) {
        uint8_t *mem = (uint8_t *)malloc(buffer.size());
        memcpy(mem, buffer.data(), buffer.size());
        return s.queue_write(id, mem, buffer.size());
    });
    
    mocktcp_lib.def("wait_events", [](mtcp_wrapper &s, std::vector<int> wait_ids) {
        auto ur = s.wait_event_one(wait_ids);
        if(!ur)
            throw std::runtime_error(std::string(Format("%s : wait_event_one timed out / interrupted", __PRETTY_FUNCTION__).buf));

        auto r = ur->r;
        auto eid = mtcp_req_get_wait_id(r);
        req_type_t req = mtcp_req_get_req_type(r);
        py::tuple ret;
    
        if(req == REQ_TYPE_READ)
            return py::make_tuple(eid, mtcp_req_get_id(r), std::string("read"), py::bytes(std::string((const char *)mtcp_req_get_buffer(r), mtcp_req_get_size(r))));
        else if(req == REQ_TYPE_WRITE && mtcp_req_has_error(r))
            return py::make_tuple(eid, mtcp_req_get_id(r), std::string("werror"), py::make_tuple(mtcp_req_get_error_info(r), mtcp_req_get_req_size(r)));
        else
            return py::make_tuple(eid, mtcp_req_get_id(r), std::string("write"), py::none());
    });
}


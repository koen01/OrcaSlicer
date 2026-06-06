#include "CrealityPrint.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fstream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using std::to_string;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

CrealityPrint::CrealityPrint(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* CrealityPrint::get_name() const { return "Creality Print"; }

std::string CrealityPrint::get_host() const {
    return m_host;
}
void  CrealityPrint::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

wxString CrealityPrint::get_test_ok_msg() const { return _(L("Connected to CrealityPrint successfully!")); }

wxString CrealityPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to CrealityPrint"), msg.Truncate(256));
}

bool CrealityPrint::test(wxString& msg) const
{ 
    bool res = true;
    const char* name = get_name();
    auto url = make_url("info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.timeout_max(5)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            try {
                auto info = json::parse(body);
                if (info.contains("model")) {
                    m_model = info["model"].get<std::string>();
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Detected model: %2%") % name % m_model;
                }
            } catch (const json::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Failed to parse /info response: %2%") % name % e.what();
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

PrintHostPostUploadActions CrealityPrint::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool CrealityPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    auto url = make_url("upload/" + safe_filename(upload_filename.string()));

    auto  http = Http::post(url); // std::move(url));
    set_auth(http);
    if (!supports_multi_color_print())
        http.form_add("path", upload_parent_path.string());
    http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                wxString errormsg;
                if (!start_print(errormsg, safe_filename(upload_filename.string()), upload_data.extended_info)) {
                    error_fn(std::move(errormsg));
                    res = false;
                }
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

std::string CrealityPrint::make_url(const std::string &path) const
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("http://%1%/%2%") % m_host % path).str();
    }
}

std::string CrealityPrint::safe_filename(const std::string &filename) const
{
    std::string safe_filename = filename;
    std::replace(safe_filename.begin(), safe_filename.end(), ' ', '_');

    return safe_filename;
}

static void ws_connect(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws,
                       const std::string& host_url, const std::string& port)
{
    std::string host = Http::get_host_from_url(host_url);

    tcp::resolver resolver{ioc};
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    auto const results = resolver.resolve(host, port);
    beast::get_lowest_layer(ws).connect(results);
    host += ':' + std::to_string(beast::get_lowest_layer(ws).socket().remote_endpoint().port());

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(http::field::user_agent,
                std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
        }));
    ws.handshake(host, "/");

    // K1-family firmware requires WebSocket TEXT frames; K2-family accepts both.
    ws.text(true);

    // Leave expires_after management to the caller (per-phase). Do NOT use
    // SO_RCVTIMEO here: on macOS/Linux Boost.Asio uses kqueue/epoll internally,
    // so socket-level SO_RCVTIMEO has no effect on Boost.Asio operations.
}

static std::string ws_send_and_read(websocket::stream<beast::tcp_stream>& ws, const json& cmd, const std::string& expected_key, int max_reads = 20)
{
    ws.write(net::buffer(to_string(cmd)));

    // total_reads caps the loop if the printer sends a continuous stream of heartbeats
    for (int reads = 0, total = 0; reads < max_reads && total < max_reads * 3; ++total) {
        beast::flat_buffer buf;
        beast::error_code ec;
        ws.read(buf, ec);
        // would_block = SO_RCVTIMEO fired (Windows/raw sockets)
        // beast::error::timeout = expires_after() fired (Boost.Beast timer, macOS/Linux)
        if (ec == net::error::would_block || ec == beast::error::timeout)
            break;
        if (ec)
            throw beast::system_error{ec};
        std::string msg = beast::buffers_to_string(buf.data());
        // K1-family firmware sends periodic heartbeat pings while we wait for data.
        // Ack them immediately so the printer does not close the connection.
        if (msg.find("heart_beat") != std::string::npos) {
            beast::error_code wr_ec;
            ws.write(net::buffer(std::string("ok")), wr_ec);
            continue; // don't count toward reads limit
        }
        ++reads;
        if (msg.find(expected_key) != std::string::npos)
            return msg;
    }
    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: No '" << expected_key << "' response after " << max_reads << " messages";
    return {};
}

void CrealityPrint::query_model() const
{
    if (!m_model.empty())
        return;

    wxString msg;
    test(msg);
}

bool CrealityPrint::supports_multi_color_print() const
{
    query_model();
    // K2-platform printers with CFS support
    return m_model == "F008"    // K2 Plus
        || m_model == "F012"    // K2 Pro
        || m_model == "F021"    // K2
        || m_model == "K1 SE";  // K1 SE
}

std::string CrealityPrint::model_name() const
{
    static const std::map<std::string, std::string> names = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F021", "K2"},
        {"K1 SE", "K1 SE"},
    };
    query_model();
    if (m_model.empty())
        return "unreachable";
    auto it = names.find(m_model);
    return it != names.end() ? it->second : "unknown (" + m_model + ")";
}

std::string CrealityPrint::query_boxes_info() const
{
    BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: connecting to " << m_host << " port 9999";
    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: WS handshake complete";

        // Step 1: Drain one initial status frame that K1-family firmware pushes right
        // after the handshake. The printer ignores commands until this exchange completes.
        // K1 SE sends frames every ~4 s; we wait for exactly one (8 s timeout).
        // Note: SO_RCVTIMEO has no effect with Boost.Asio on macOS/Linux (kqueue/epoll);
        // expires_after() on tcp_stream is used instead, but it only fires before the
        // NEXT operation — it cannot interrupt an in-progress ws.read().
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(8));
        {
            beast::flat_buffer buf; beast::error_code ec;
            ws.read(buf, ec);
            if (!ec) {
                std::string frame = beast::buffers_to_string(buf.data());
                BOOST_LOG_TRIVIAL(debug) << "CrealityPrint::query_boxes_info: burst frame: "
                    << frame.substr(0, 120);
                if (frame.find("heart_beat") != std::string::npos) {
                    beast::error_code we; ws.write(net::buffer(std::string("ok")), we);
                }
            } else {
                BOOST_LOG_TRIVIAL(debug) << "CrealityPrint::query_boxes_info: burst done ("
                    << ec.message() << ")";
            }
        }

        // Step 2: Client-initiated heartbeat handshake — 5 s for the exchange.
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: sending heartbeat";
        ws.write(net::buffer(to_string(json{{"ModeCode", "heart_beat"}})));
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: heartbeat write OK, waiting for ok";

        bool got_hb_ok = false;
        for (int i = 0; i < 10; i++) {
            beast::flat_buffer buf; beast::error_code ec;
            ws.read(buf, ec);
            if (ec) {
                BOOST_LOG_TRIVIAL(warning) << "CrealityPrint::query_boxes_info: heartbeat read[" << i
                    << "] error: " << ec.message();
                break;
            }
            std::string reply = beast::buffers_to_string(buf.data());
            BOOST_LOG_TRIVIAL(debug) << "CrealityPrint::query_boxes_info: heartbeat reply[" << i
                << "]: " << reply;
            std::string trimmed = reply;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            if (!trimmed.empty()) trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
            if (trimmed == "ok") { got_hb_ok = true; break; }
            if (reply.find("heart_beat") != std::string::npos) {
                beast::error_code we; ws.write(net::buffer(std::string("ok")), we);
            }
        }
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: heartbeat phase done"
            << (got_hb_ok ? " (got ok)" : " (no ok)");

        // Step 3: Request box info — 10 s for request + response.
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: sending boxsInfo request";
        json boxs_query = {{"method", "get"}, {"params", {{"boxsInfo", 1}}}};
        std::string result = ws_send_and_read(ws, boxs_query, "boxsInfo");
        BOOST_LOG_TRIVIAL(info) << "CrealityPrint::query_boxes_info: boxsInfo result size=" << result.size();

        // Close gracefully; the K1 SE closes its side after sending the response, so
        // ws.close() will get EOF — use the error_code overload to avoid throwing and
        // discarding the already-received result.
        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.close(websocket::close_code::normal, close_ec);
        if (close_ec)
            BOOST_LOG_TRIVIAL(debug) << "CrealityPrint::query_boxes_info: close: " << close_ec.message();

        return result;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to query boxsInfo: " << e.what();
        return {};
    }
}

std::string CrealityPrint::get_print_host_webui(DynamicPrintConfig* config)
{
    // K-series printers (K2 / K2 Plus / K2 Pro) ship with Mainsail on port 4408.
    // Port 80 hosts only the Creality control / upload API, which returns 404
    // for unknown paths and therefore renders as a blank/404 page in Orca's
    // Device WebView. Default to the Mainsail URL when the user hasn't
    // explicitly set print_host_webui.
    if (config == nullptr)
        return {};

    std::string explicit_url = config->opt_string("print_host_webui");
    if (!explicit_url.empty())
        return explicit_url;

    std::string host = config->opt_string("print_host");
    if (host.empty())
        return {};

    if (boost::algorithm::istarts_with(host, "http://"))
        host = host.substr(7);
    else if (boost::algorithm::istarts_with(host, "https://"))
        host = host.substr(8);
    if (auto slash = host.find('/'); slash != std::string::npos)
        host = host.substr(0, slash);
    if (auto colon = host.find(':'); colon != std::string::npos)
        host = host.substr(0, colon);

    return "http://" + host + ":4408/";
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename, const std::map<std::string, std::string>& extended_info) const
{
    try {
        // K1-family firmware (K1 SE) stores gcodes under /usr/data; K2-family uses /mnt/UDISK.
        const std::string data_root = (m_model == "K1 SE") ? "/usr/data" : "/mnt/UDISK";
        const std::string gcode_path = data_root + "/printer_data/gcodes/" + filename;

        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        // K1-family requires reading one initial status frame then a heartbeat
        // handshake before it accepts print commands — same as query_boxes_info().
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(8));
        {
            beast::flat_buffer buf; beast::error_code ec;
            ws.read(buf, ec);
            if (!ec) {
                std::string frame = beast::buffers_to_string(buf.data());
                if (frame.find("heart_beat") != std::string::npos) {
                    beast::error_code we; ws.write(net::buffer(std::string("ok")), we);
                }
            }
        }
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
        ws.write(net::buffer(to_string(json{{"ModeCode", "heart_beat"}})));
        for (int i = 0; i < 10; i++) {
            beast::flat_buffer buf; beast::error_code ec;
            ws.read(buf, ec);
            if (ec) break;
            std::string reply = beast::buffers_to_string(buf.data());
            reply.erase(0, reply.find_first_not_of(" \t\r\n"));
            if (!reply.empty()) reply.erase(reply.find_last_not_of(" \t\r\n") + 1);
            if (reply == "ok") break;
            if (reply.find("heart_beat") != std::string::npos) {
                beast::error_code we; ws.write(net::buffer(std::string("ok")), we);
            }
        }

        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        if (supports_multi_color_print()) {
            // Build colorMatch list from the mapping provided by the dialog
            bool use_spool_holder = false;
            json color_list = json::array();
            for (int i = 0; ; i++) {
                auto it = extended_info.find("colorMatch_" + std::to_string(i));
                if (it == extended_info.end())
                    break;
                // Value format: "toolId\ttype\tcolor\tboxId\tmaterialId"
                auto val = it->second;
                std::vector<std::string> parts;
                std::istringstream iss(val);
                std::string part;
                while (std::getline(iss, part, '\t'))
                    parts.push_back(part);
                if (parts.size() >= 5) {
                    int box_id = std::stoi(parts[3]);
                    if (box_id == 0)
                        use_spool_holder = true;
                    color_list.push_back({
                        {"id", parts[0]},
                        {"type", parts[1]},
                        {"color", parts[2]},
                        {"boxId", box_id},
                        {"materialId", std::stoi(parts[4])}
                    });
                }
            }

            int enable_self_test = 0;
            {
                auto it = extended_info.find("enableSelfTest");
                if (it != extended_info.end())
                    enable_self_test = std::stoi(it->second);
            }

            if (use_spool_holder) {
                json cmd = {
                    {"method", "set"},
                    {"params", {
                        {"opGcodeFile", "printprt:" + gcode_path},
                        {"enableSelfTest", enable_self_test}
                    }}
                };
                ws.write(net::buffer(to_string(cmd)));
            } else {
                json color_match = {
                    {"method", "set"},
                    {"params", {
                        {"colorMatch", {
                            {"path", gcode_path},
                            {"list", color_list}
                        }}
                    }}
                };
                ws.write(net::buffer(to_string(color_match)));

                json multi_color_print = {
                    {"method", "set"},
                    {"params", {
                        {"multiColorPrint", {
                            {"gcode", gcode_path},
                            {"enableSelfTest", enable_self_test}
                        }}
                    }}
                };
                ws.write(net::buffer(to_string(multi_color_print)));
            }
        } else {
            json cmd = {
                {"method", "set"},
                {"params", {
                    {"opGcodeFile", "printprt:/usr/data/printer_data/gcodes/" + filename}
                }}
            };
            ws.write(net::buffer(to_string(cmd)));

            beast::flat_buffer buffer;
            ws.read(buffer);
        }

        beast::error_code close_ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.close(websocket::close_code::normal, close_ec);
        return true;
    } catch(std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}

/*<one line to give the program's name and a brief idea of what it does.>
Copyright (C) <year>  <name of author>

This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/error.hpp>

#include <boost/utility/string_view.hpp>
#include <boost/fiber/all.hpp>

#include "yield.hpp"
#include "round_robin.hpp"

/*!
 * @brief webxx is a simple HTTP framework, providing basic servers, clients, and routers.
 * The desire is to be able to build a flexible base which can quickly be built from,
 * while still giving enough power to do work.
 *
 * @details The code is built around using boost::asio and boost::fiber.  The combination
 * gives a async model that can be expressed in linear looking code.
 */
namespace webxx
{
    using io_context = boost::asio::io_context;

    class Server;
    class Connection;

    /*!
     * @brief A route is a match between a url pattern and a handler.
     * @details Route is the base class to allow a more flexible set of matchers.
     */
    class Route
    {
    public:
        virtual ~Route() = default;
        virtual bool matches(boost::beast::http::verb method, boost::string_view path) = 0;
        virtual void operator()(Connection& conn) = 0;
    };

    /*!
     * @brief A Router contains a collection of Routes and given a request provides the right handler.
     */
    class Router
    {
    public:
        Router() = default;
        Router(const Router& other) = delete;
        Router(Router&& other) = default;
        Router& operator=(const Router& other) = delete;
        Router& operator=(Router&& other) = default;

        template<typename T, typename ... Args>
        void
        add(Args&&... args)
        {
            routes_.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
        }

        bool operator()(Connection& conn, boost::beast::http::verb method, boost::string_view path)
        {
            auto it = std::find_if(routes_.begin(), routes_.end(), [method, path](const std::unique_ptr<Route>& route)-> bool{
               return route->matches(method, path);
            });
            if (it == routes_.end())
            {
                return false;
            }
            (*it)->operator()(conn);
            return true;
        }
    private:
        std::vector< std::unique_ptr<Route> > routes_;
    };

    /*!
     * @brief The model of a generic HTTP connection handler.
     * @detail The Connection manages the lifetime of the socket, the parsing of the input
     * the dispatching to a handler.
     * The Connection should be managed by a shared_ptr.  When all references to the Connection
     * are released, the connection is automatically closed down.
     */
    class Connection: public std::enable_shared_from_this< Connection >
    {
    public:
        using socket_type = boost::asio::ip::tcp::socket;

        /*!
         * @brief Used to create a connection
         * @param context The asio context needed to do network work
         * @param router The router to reference when a request is handled
         * @note This should be made private some way, to enforce the creation of
         * the connection with a shared ptr.
         */
        explicit Connection(io_context& context, Router& router): s_{context},
        router_{router}, read_buf_() {}

        ~Connection()
        {
            std::cerr << "Connection closing" << std::endl;
        }

        /*!
         * @brief Retrieve the network socket
         * @return the socket
         */
        socket_type& socket()
        {
            return s_;
        }

        /*!
         * @brief start the connection, it must have a valid socket.
         */
        void run()
        {
            read_request();
        }
    private:
        void read_request()
        {
            boost::beast::http::request_parser<boost::beast::http::string_body> parser;
            do
            {
                boost::system::error_code ec;
                std::size_t length = s_.async_read_some(read_buf_.prepare(1024), boost::fibers::asio::yield[ec]);
                if (ec)
                {
                    return;
                }
                read_buf_.commit(length);
                ec.clear();
                auto bytes_used = parser.put(read_buf_.data(), ec);
                if (ec == boost::beast::http::error::need_more)
                {
                    ec = {};
                }
                if (ec)
                {
                    return;
                }
                read_buf_.consume( bytes_used );
            }
            while( ! parser.is_done() );
            msg_ = parser.get();
            dispatch_read();
        }

        void dispatch_read()
        {
            std::cout << "Got a message\n";
            std::cout << "\tmethod: " << msg_.method_string() << "\n";
            std::cout << "\ttarget: " << msg_.target() << "\n";
            msg_path_ = msg_.target().substr(0, msg_.target().find_first_of('?'));
            std::cout << "\tpath: " << msg_path_ << "\n";

            router_(*this, msg_.method(), msg_path_);
        }

        boost::asio::ip::tcp::socket s_;
        Router& router_;
        boost::beast::flat_buffer read_buf_;
        boost::beast::http::request<boost::beast::http::string_body> msg_;
        boost::string_view msg_path_;
    };

    /*!
     * @brief a generic server.  It has no knowledge of the client protocol.
     * @detail The server simply manages the server socket and passes all details of
     * handling on to the Connection objects.
     */
    class Server
    {
    public:
        explicit Server(int threads = 1) : contexts_(create_contexts(threads)),
        work_(create_work_guard(contexts_)),
        threads_(threads),
        acceptor_(*(contexts_[0]), boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 9000))
        {}

        void run()
        {
            for (int i = 1; i < contexts_.size(); ++i)
            {
                threads_[i] = std::thread([this, i](){
                    this->run_thread(i);
                });
            }
            run_thread(0);
        }

        Router& router()
        {
            return router_;
        }

    private:
        using acceptor_type = boost::asio::ip::tcp::acceptor;
        using context_ptr = std::shared_ptr<io_context>;
        using work_guard = boost::asio::executor_work_guard<io_context::executor_type>;

        void run_thread(int n)
        {
            boost::fibers::use_scheduling_algorithm<boost::fibers::asio::round_robin>(contexts_[n]);

            if (n == 0)
            {
                boost::fibers::fiber([this]() {
                    accept_loop();
                }).detach();
            }
            contexts_[n]->run();
        }

        static std::vector<context_ptr> create_contexts(int count)
        {
            std::vector<context_ptr> ctxs;
            ctxs.reserve(count);
            for (auto i = 0; i < count; ++i)
            {
                ctxs.emplace_back(std::make_shared<io_context>());
            }
            return ctxs;
        }

        static std::vector<work_guard> create_work_guard(std::vector<context_ptr>& ctxs)
        {
            std::vector<work_guard> guards;
            guards.reserve(ctxs.size());
            std::for_each(ctxs.begin(), ctxs.end(), [&guards](context_ptr& ctx) {
                guards.emplace_back(boost::asio::make_work_guard(*ctx));
            });
            return guards;
        }

        void
        accept_loop()
        {
            int selector = 0;
            int max = contexts_.size();
            while (true)
            {
                auto conn = std::make_shared<Connection>(*contexts_[selector], router_);
                boost::system::error_code ec;
                acceptor_.async_accept(conn->socket(),
                    boost::fibers::asio::yield[ec]);
                if ( ec )
                {
                    throw boost::system::system_error( ec );
                }

                contexts_[selector]->post([conn=std::move(conn)]() mutable
                {
                    try {
                        boost::fibers::fiber([conn=std::move(conn)]() mutable {
                            conn->run();
                        }).detach();
                    }
                    catch(...)
                    {
                    }
                });

                selector++;
                selector %= max;
            }
        }


        std::vector< context_ptr > contexts_;
        std::vector< boost::asio::executor_work_guard<io_context::executor_type> > work_;
        std::vector< std::thread > threads_;
        acceptor_type acceptor_;
        Router router_;
    };
}

/*!
 * @brief a test route.
 */
class StaticRoute : public webxx::Route
{
public:
    using handler_type = std::function<void(webxx::Connection&)>;

    StaticRoute(boost::beast::http::verb method, std::string path, handler_type&& handler):
    method_{method}, path_{std::move(path)}, handler_{std::move(handler)} {}
    StaticRoute(StaticRoute&& other) = default;
    ~StaticRoute() override = default;

    bool matches(boost::beast::http::verb method, boost::string_view path) override
    {
        return method == method_ && path == path_;
    }

    void operator()(webxx::Connection& conn) override
    {
        handler_(conn);
    }

private:
    boost::beast::http::verb method_;
    std::string path_;
    handler_type handler_;
};

void index_handler(webxx::Connection &conn)
{
    namespace http = boost::beast::http;

    std::cout << "Answering request in thread" << std::this_thread::get_id() << std::endl;

    std::string body = "<html><body><p>The index!</p></body></html>";

    http::response<http::string_body> resp;
    resp.version(11);
    resp.result(http::status::ok);
    resp.set(http::field::content_length, body.size());
    resp.body() = body;
    boost::system::error_code ec;

    boost::beast::http::response_serializer<boost::beast::http::string_body> sr{resp};
    do
    {
        sr.next(ec, [&sr, &conn](boost::system::error_code &ec, const auto &buffer)
        {
            ec.assign(0, ec.category());
            boost::system::error_code wec;
            boost::asio::async_write(conn.socket(), buffer,
                                     boost::fibers::asio::yield[wec]);
            if (wec)
            {
                throw boost::system::error_code(ec);
            }
            sr.consume(boost::asio::buffer_size(buffer));
        });
    } while (!ec && !sr.is_done());
}

void not_found(webxx::Connection &conn)
{
    namespace http = boost::beast::http;

    std::cout << "Not found request in thread" << std::this_thread::get_id() << std::endl;

    std::string body = "<html><body><p>Not Found!</p></body></html>";

    http::response<http::string_body> resp;
    resp.version(11);
    resp.result(http::status::not_found);
    resp.set(http::field::content_length, body.size());
    resp.body() = body;
    boost::system::error_code ec;

    boost::beast::http::response_serializer<boost::beast::http::string_body> sr{resp};
    do
    {
        sr.next(ec, [&sr, &conn](boost::system::error_code &ec, const auto &buffer)
        {
            ec.assign(0, ec.category());
            boost::system::error_code wec;
            boost::asio::async_write(conn.socket(), buffer,
                                     boost::fibers::asio::yield[wec]);
            if (wec)
            {
                throw boost::system::error_code(ec);
            }
            sr.consume(boost::asio::buffer_size(buffer));
        });
    } while (!ec && !sr.is_done());
}

int main()
{
  std::cout << "Hello, World!" << std::endl;
  webxx::Server server(3);
  server.router().add<StaticRoute>(boost::beast::http::verb::get, "/", index_handler);
  server.router().add<StaticRoute>(boost::beast::http::verb::get, "/favicon.ico", not_found);
  server.run();
  return 0;
}

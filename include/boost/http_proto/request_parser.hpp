//
// Copyright (c) 2019 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/CPPAlliance/http_proto
//

#ifndef BOOST_HTTP_PROTO_REQUEST_PARSER_HPP
#define BOOST_HTTP_PROTO_REQUEST_PARSER_HPP

#include <boost/http_proto/detail/config.hpp>
#include <boost/http_proto/error.hpp>
#include <boost/http_proto/method.hpp>
#include <boost/http_proto/parser.hpp>
#include <boost/http_proto/request_view.hpp>
#include <boost/http_proto/string_view.hpp>
#include <cstddef>
#include <utility>

namespace boost {
namespace http_proto {

class BOOST_SYMBOL_VISIBLE
    request_parser
    : public parser
{
public:
    /** Configuration settings for parsing requests
    */
    struct config : config_base
    {
        /** Constructor
        */
        config() noexcept
        {
            max_body_size = 64 * 1024;
        }
    };

    /** Constructor
    */
    BOOST_HTTP_PROTO_DECL
    request_parser();

    /** Constructor
    */
    template<class... Params>
    explicit
    request_parser(
        std::size_t extra_buffer_size,
        Params&&... params)
        : parser(
            detail::kind::request,
            config{})
    {
        this->apply_params(
            std::forward<Params>(params)...);
        construct(extra_buffer_size);
    }

    /** Prepare for the next message on the stream.
    */
    void
    start()
    {
        start_impl(false);
    }

    /** Return the parsed request headers.
    */
    BOOST_HTTP_PROTO_DECL
    request_view
    get() const noexcept;
};

} // http_proto
} // boost

#endif

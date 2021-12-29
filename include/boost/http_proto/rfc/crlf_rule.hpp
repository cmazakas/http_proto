//
// Copyright (c) 2021 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/CPPAlliance/http_proto
//

#ifndef BOOST_HTTP_PROTO_RFC_CRLF_RULE_HPP
#define BOOST_HTTP_PROTO_RFC_CRLF_RULE_HPP

#include <boost/http_proto/detail/config.hpp>
#include <boost/http_proto/error.hpp>
#include <boost/url/grammar/error.hpp>

namespace boost {
namespace http_proto {

/** Rule for CRLF

    @par BNF
    @code
    CRLF            = CR LF
    @endcode
*/
struct crlf_rule
{
    inline
    friend
    bool
    parse(
        char const*& it,
        char const* end,
        error_code& ec,
        crlf_rule const&) noexcept;
};

//------------------------------------------------

bool
parse(
    char const*& it,
    char const* end,
    error_code& ec,
    crlf_rule const&) noexcept
{
    if(it == end)
    {
        ec = grammar::error::incomplete;
        return false;
    }
    if(it[0] != '\r')
    {
        ec = grammar::error::syntax;
        return false;
    }
    if(it + 1 == end)
    {
        ec = grammar::error::incomplete;
        return false;
    }
    if(it[1] != '\n')
    {
        ec = grammar::error::syntax;
        return false;
    }
    it += 2;
    return true;
}

} // http_proto
} // boost

#endif

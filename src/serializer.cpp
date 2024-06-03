//
// Copyright (c) 2019 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2024 Christian Mazakas
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/http_proto
//

#include <boost/http_proto/serializer.hpp>
#include <boost/http_proto/message_view_base.hpp>
#include <boost/http_proto/filter.hpp>
#include <boost/http_proto/service/zlib_service.hpp>
#include <boost/http_proto/detail/except.hpp>
#include <boost/buffers/algorithm.hpp>
#include <boost/buffers/buffer_copy.hpp>
#include <boost/buffers/buffer_size.hpp>
#include <boost/core/ignore_unused.hpp>
#include <stddef.h>

namespace boost {
namespace http_proto {

//------------------------------------------------

void
consume_buffers(
    buffers::const_buffer*& p,
    std::size_t& n,
    std::size_t bytes)
{
    while(n > 0)
    {
        if(bytes < p->size())
        {
            *p += bytes;
            return;
        }
        bytes -= p->size();
        ++p;
        --n;
    }

    // Precondition violation
    if(bytes > 0)
        detail::throw_invalid_argument();
}

template<class MutableBuffers>
void
write_chunk_header(
    MutableBuffers const& dest0,
    std::size_t size) noexcept
{
    static constexpr char hexdig[] =
        "0123456789ABCDEF";
    char buf[18];
    auto p = buf + 16;
    for(std::size_t i = 16; i--;)
    {
        *--p = hexdig[size & 0xf];
        size >>= 4;
    }
    buf[16] = '\r';
    buf[17] = '\n';
    auto n = buffers::buffer_copy(
        dest0,
        buffers::const_buffer(
            buf, sizeof(buf)));
    ignore_unused(n);
    BOOST_ASSERT(n == 18);
    BOOST_ASSERT(
        buffers::buffer_size(dest0) == n);
}

template<class DynamicBuffer>
void
write_chunk_close(DynamicBuffer& db)
{
    db.commit(
        buffers::buffer_copy(
            db.prepare(2),
            buffers::const_buffer("\r\n", 2)));
}

template<class DynamicBuffer>
void
write_last_chunk(DynamicBuffer& db)
{
    db.commit(
        buffers::buffer_copy(
            db.prepare(5),
            buffers::const_buffer("0\r\n\r\n", 5)));
}

//------------------------------------------------

serializer::
~serializer()
{
}

serializer::
serializer(
    serializer&&) noexcept = default;

serializer::
serializer(
    context& ctx)
    : serializer(ctx, 65536)
{
}

serializer::
serializer(
    context& ctx,
    std::size_t buffer_size)
    : ws_(buffer_size)
    , ctx_(ctx)
{
}

void
serializer::
reset() noexcept
{
    ws_.clear();
    more_ = false;
    is_done_ = false;
    is_chunked_ = false;
    is_expect_continue_ = false;
    is_compressed_ = false;
    filter_done_ = false;
}

//------------------------------------------------

auto
serializer::
prepare() ->
    system::result<
        const_buffers_type>
{
    // Precondition violation
    if(is_done_)
        detail::throw_logic_error();

    // Expect: 100-continue
    if(is_expect_continue_)
    {
        if(out_.data() == hp_)
            return const_buffers_type(hp_, 1);
        is_expect_continue_ = false;
        BOOST_HTTP_PROTO_RETURN_EC(
            error::expect_100_continue);
    }

    if( is_compressed_ )
    {
        BOOST_ASSERT(deflate_filter_);

        auto& zbuf = tmp1_;

        auto on_end = [&]
        {
            std::size_t n = 0;
            if(out_.data() == hp_)
                ++n;

            for(buffers::const_buffer const& b : tmp0_.data())
                out_[n++] = b;

            auto cbs = const_buffers_type(
                out_.data(), out_.size());

            BOOST_ASSERT(buffers::buffer_size(cbs) > 0);

            return cbs;
        };

        auto get_input = [&]() -> buffers::const_buffer
        {
            if( st_ == style::buffers )
            {
                if( buffers::buffer_size(buf_) == 0 )
                    return {};

                auto buf = *(buf_.data());
                BOOST_ASSERT(buf.size() > 0);
                return buf;
            }
            else
            {
                if( zbuf.size() == 0 )
                    return {};

                auto cbs = zbuf.data();
                auto buf = *cbs.begin();
                if( buf.size() == 0 )
                {
                    auto p = cbs.begin();
                    ++p;
                    buf = *p;
                }
                BOOST_ASSERT(buf.size() > 0);
                return buf;
            }
        };

        auto get_output = [&]() -> buffers::mutable_buffer
        {
            auto mbs = tmp0_.prepare(tmp0_.capacity());
            auto buf = *mbs.begin();
            if( buf.size() == 0 )
            {
                auto p = mbs.begin();
                ++p;
                buf = *p;
            }

            if( is_chunked_ )
            {
                if( buf.size() <
                    crlf_len_ + last_chunk_len_ + 1 )
                    return {};

                auto n =
                    buf.size() -
                    crlf_len_ -
                    last_chunk_len_;

                buf = { buf.data(), n };
            }
            return buf;
        };

        auto consume = [&](std::size_t n)
        {
            if( st_ == style::buffers )
            {
                buf_.consume(n);
                if( buffers::buffer_size(buf_) == 0 )
                    more_ = false;
            }
            else
                zbuf.consume(n);
        };

        // callers must consume() everything before invoking
        // prepare() again
        if( tmp0_.size() > 0 )
            detail::throw_logic_error();

        // minimum output buffer size has to accommodate
        // chunked overhead, zlib flush marker overhead and
        // must have space for at least one output byte
        if( tmp0_.capacity() < chunked_overhead_ + 6 + 1 )
            detail::throw_length_error();

        if( st_ == style::source )
        {
            auto results = src_->read(
                zbuf.prepare(zbuf.capacity()));
            more_ = !results.finished;
            zbuf.commit(results.bytes);
        }

        buffers::mutable_buffer chunk_header;
        if( is_chunked_ )
        {
            auto mbs =
                tmp0_.prepare(chunk_header_len_);
            auto buf = *mbs.begin();
            if( buf.size() == 0 )
            {
                auto p = mbs.begin();
                ++p;
                buf = *p;
            }

            // user hasn't called `.consume()` enough for
            // our buffers to have enough space to write
            // the chunk header out to
            if( buf.size() < chunk_header_len_ )
                detail::throw_logic_error();

            std::memset(
                buf.data(), 0, chunk_header_len_);
            tmp0_.commit(chunk_header_len_);

            chunk_header = buf;
        }

        std::size_t num_written = 0;
        for(;;)
        {
            auto in = get_input();
            auto out = get_output();
            if( out.size() == 0 )
            {
                if( tmp0_.size() == 0 )
                    detail::throw_logic_error();
                break;
            }

            auto results = deflate_filter_->on_process(
                out, in, more_);

            if( results.finished )
                filter_done_ = true;

            consume(results.in_bytes);

            if( results.out_bytes == 0 )
                break;

            num_written += results.out_bytes;
            tmp0_.commit(results.out_bytes);
        }

        BOOST_ASSERT(tmp0_.size() > 0);
        if( is_chunked_ )
        {
            write_chunk_header(
                chunk_header, num_written);

            buffers::buffer_copy(
                tmp0_.prepare(2),
                buffers::const_buffer("\r\n", 2));
            tmp0_.commit(2);

            if( filter_done_ )
            {
                buffers::buffer_copy(
                    tmp0_.prepare(5),
                    buffers::const_buffer(
                        "0\r\n\r\n", 5));
                tmp0_.commit(5);
            }
        }
        return on_end();
    }

    if(st_ == style::empty)
    {
        return const_buffers_type(
            out_.data(),
            out_.size());
    }

    if(st_ == style::buffers)
    {
        return const_buffers_type(
            out_.data(),
            out_.size());
    }

    if(st_ == style::source)
    {
        if(more_)
        {
            if(! is_chunked_)
            {
                auto rv = src_->read(
                    tmp0_.prepare(tmp0_.capacity()));
                tmp0_.commit(rv.bytes);
                if(rv.ec.failed())
                    return rv.ec;
                more_ = ! rv.finished;
            }
            else
            {
                if( tmp0_.capacity() > chunked_overhead_ )
                {
                    auto dest = tmp0_.prepare(
                        tmp0_.capacity() -
                        2 - // CRLF
                        5); // final chunk

                    auto rv = src_->read(
                        buffers::sans_prefix(dest, 18));

                    if(rv.ec.failed())
                        return rv.ec;

                    if(rv.bytes != 0)
                    {
                        write_chunk_header(
                            buffers::prefix(dest, 18), rv.bytes);
                        tmp0_.commit(rv.bytes + 18);
                        // terminate chunk
                        tmp0_.commit(
                            buffers::buffer_copy(
                                tmp0_.prepare(2),
                                buffers::const_buffer(
                                    "\r\n", 2)));
                    }

                    if(rv.finished)
                    {
                        tmp0_.commit(
                            buffers::buffer_copy(
                                tmp0_.prepare(5),
                                buffers::const_buffer(
                                    "0\r\n\r\n", 5)));
                        more_ = false;
                    }
                }
            }
        }

        std::size_t n = 0;
        if(out_.data() == hp_)
            ++n;
        for(buffers::const_buffer const& b : tmp0_.data())
            out_[n++] = b;

        return const_buffers_type(
            out_.data(),
            out_.size());
    }

    if(st_ == style::stream)
    {
        std::size_t n = 0;
        if(out_.data() == hp_)
            ++n;
        if(tmp0_.size() == 0 && more_)
        {
            BOOST_HTTP_PROTO_RETURN_EC(
                error::need_data);
        }
        for(buffers::const_buffer const& b : tmp0_.data())
            out_[n++] = b;

        return const_buffers_type(
            out_.data(),
            out_.size());
    }

    // should never get here
    detail::throw_logic_error();
}

void
serializer::
consume(
    std::size_t n)
{
    // Precondition violation
    if(is_done_)
        detail::throw_logic_error();

    if(is_expect_continue_)
    {
        // Cannot consume more than
        // the header on 100-continue
        if(n > hp_->size())
            detail::throw_invalid_argument();

        out_.consume(n);
        return;
    }
    else if(out_.data() == hp_)
    {
        // consume header
        if(n < hp_->size())
        {
            out_.consume(n);
            return;
        }
        n -= hp_->size();
        out_.consume(hp_->size());
    }

    switch(st_)
    {
    default:
    case style::empty:
        out_.consume(n);
        if(out_.empty())
            is_done_ = true;
        return;

    case style::buffers:
        if( is_compressed_ )
        {
            tmp0_.consume(n);
            if( tmp0_.size() == 0 &&
                filter_done_ )
                is_done_ = true;
            return;
        }
        out_.consume(n);
        if(out_.empty())
            is_done_ = true;
        return;

    case style::source:
    case style::stream:
        tmp0_.consume(n);
        if( !is_compressed_ &&
            tmp0_.size() == 0 &&
            ! more_)
            is_done_ = true;

        if( is_compressed_ &&
            tmp0_.size() == 0 &&
            filter_done_ )
            is_done_ = true;
        return;
    }
}

//------------------------------------------------

void
serializer::
copy(
    buffers::const_buffer* dest,
    buffers::const_buffer const* src,
    std::size_t n) noexcept
{
    while(n--)
        *dest++ = *src++;
}

void
serializer::
start_init(
    message_view_base const& m)
{
    reset();

    // VFALCO what do we do with
    // metadata error code failures?
    // m.ph_->md.maybe_throw();

    is_done_ = false;

    is_expect_continue_ =
        m.ph_->md.expect.is_100_continue;

    // Transfer-Encoding
    {
        auto const& te =
            m.ph_->md.transfer_encoding;
        is_chunked_ = te.is_chunked;
    }

    if( m.metadata().transfer_encoding.encoding !=
        http_proto::encoding::identity )
    {
        is_compressed_ = true;
        auto& svc =
            ctx_.get_service<
                zlib::deflate_decoder_service>();

        auto use_gzip =
            m.metadata().transfer_encoding.encoding ==
            http_proto::encoding::gzip;

        deflate_filter_ =
            use_gzip ?
            &svc.make_gzip_filter(ws_) :
            &svc.make_deflate_filter(ws_);
    }
}

void
serializer::
start_empty(
    message_view_base const& m)
{
    start_init(m);

    st_ = style::empty;

    if(! is_chunked_)
    {
        out_ = make_array(
            1); // header
    }
    else
    {
        out_ = make_array(
            1 + // header
            1); // final chunk

        // Buffer is too small
        if(ws_.size() < 5)
            detail::throw_length_error();

        buffers::mutable_buffer dest(
            ws_.data(), 5);
        buffers::buffer_copy(
            dest,
            buffers::const_buffer(
                "0\r\n\r\n", 5));
        out_[1] = dest;
    }

    hp_ = &out_[0];
    *hp_ = { m.ph_->cbuf, m.ph_->size };
}

void
serializer::
start_buffers(
    message_view_base const& m)
{
    st_ = style::buffers;
    if( is_compressed_ )
    {
        out_ = make_array(
            1 + // header
            2); // tmp

        hp_ = &out_[0];
        *hp_ = { m.ph_->cbuf, m.ph_->size };
        tmp0_ = { ws_.data(), ws_.size() };
        more_ = true;
        return;
    }

    if(! is_chunked_)
    {
        //if(! cod_)
        {
            out_ = make_array(
                1 +             // header
                buf_.size());   // body
            copy(&out_[1],
                buf_.data(), buf_.size());
        }
#if 0
        else
        {
            out_ = make_array(
                1 + // header
                2); // tmp1
        }
#endif
    }
    else
    {
        //if(! cod_)
        {
            out_ = make_array(
                1 +             // header
                1 +             // chunk size
                buf_.size() +   // body
                1);             // final chunk
            copy(&out_[2],
                buf_.data(), buf_.size());

            // Buffer is too small
            if(ws_.size() < 18 + 7)
                detail::throw_length_error();
            buffers::mutable_buffer s1(ws_.data(), 18);
            buffers::mutable_buffer s2(ws_.data(), 18 + 7);
            s2 += 18; // VFALCO HACK
            write_chunk_header(
                s1,
                buffers::buffer_size(buf_));
            buffers::buffer_copy(s2, buffers::const_buffer(
                "\r\n"
                "0\r\n"
                "\r\n", 7));
            out_[1] = s1;
            out_[out_.size() - 1] = s2;
        }
#if 0
        else
        {
            out_ = make_array(
                1 +     // header
                2);     // tmp1
        }
#endif
    }

    hp_ = &out_[0];
    *hp_ = { m.ph_->cbuf, m.ph_->size };
}

void
serializer::
start_source(
    message_view_base const& m,
    source* src)
{
    st_ = style::source;
    src_ = src;
    out_ = make_array(
        1 + // header
        2); // tmp

    if( is_compressed_ )
    {
        auto n = ws_.size() / 2;
        auto* p = ws_.reserve_front(n);
        tmp1_ = buffers::circular_buffer(p, n);
    }

    tmp0_ = { ws_.data(), ws_.size() };
    if(tmp0_.capacity() <
            18 +    // chunk size
            1 +     // body (1 byte)
            2 +     // CRLF
            5)      // final chunk
        detail::throw_length_error();

    hp_ = &out_[0];
    *hp_ = { m.ph_->cbuf, m.ph_->size };
    more_ = true;
}

auto
serializer::
start_stream(
    message_view_base const& m) ->
        stream
{
    start_init(m);

    st_ = style::stream;
    out_ = make_array(
        1 + // header
        2); // tmp

    if( is_compressed_ )
    {
        auto n = ws_.size() / 2;
        auto* p = ws_.reserve_front(n);
        tmp1_ = buffers::circular_buffer(p, n);
    }

    tmp0_ = { ws_.data(), ws_.size() };
    if(tmp0_.capacity() <
            18 +    // chunk size
            1 +     // body (1 byte)
            2 +     // CRLF
            5)      // final chunk
        detail::throw_length_error();

    hp_ = &out_[0];
    *hp_ = { m.ph_->cbuf, m.ph_->size };
    more_ = true;
    return stream{*this};
}

//------------------------------------------------

std::size_t
serializer::
stream::
capacity() const noexcept
{
    return sr_->tmp0_.capacity();
}

std::size_t
serializer::
stream::
size() const noexcept
{
    return sr_->tmp0_.size();
}

bool
serializer::
stream::
is_full() const noexcept
{
    if( sr_->is_chunked_ )
        return capacity() < chunked_overhead_ + 1;

    return capacity() == 0;
}

auto
serializer::
stream::
prepare() const ->
    buffers_type
{
    if( sr_->is_compressed_ )
        return sr_->tmp1_.prepare(
            sr_->tmp1_.capacity());

    auto n = sr_->tmp0_.capacity();
    if( sr_->is_chunked_ )
    {
        // for chunked encoding, we want to unconditionally
        // reserve space for the complete chunk and the
        // last-chunk
        // this enables users to call:
        //
        //     stream.commit(n); stream.close();
        //
        // without needing to worry about draining the
        // serializer via `consume()` calls
        if( n < chunked_overhead_ + 1 )
            detail::throw_length_error();

        n -= chunked_overhead_;
        return buffers::sans_prefix(
            sr_->tmp0_.prepare(
                chunk_header_len_ + n),
            chunk_header_len_);
    }

    return sr_->tmp0_.prepare(n);
}

void
serializer::
stream::
commit(std::size_t n) const
{
    if( sr_->is_compressed_ )
    {
        sr_->tmp1_.commit(n);
        return;
    }

    if(! sr_->is_chunked_ )
    {
        sr_->tmp0_.commit(n);
    }
    else
    {
        // Zero sized chunks are not valid. Call close()
        // if the intent is to signal the end of the body.
        if( n == 0 )
            detail::throw_logic_error();

        auto m = n + chunk_header_len_;
        auto dest = sr_->tmp0_.prepare(m);
        write_chunk_header(
            buffers::prefix(
                dest, chunk_header_len_),
            n);
        sr_->tmp0_.commit(m);
        write_chunk_close(sr_->tmp0_);
    }
}

void
serializer::
stream::
close() const
{
    // Precondition violation
    if(! sr_->more_ )
        detail::throw_logic_error();

    if( sr_->is_chunked_ && !sr_->is_compressed_ )
        write_last_chunk(sr_->tmp0_);

    sr_->more_ = false;
}

//------------------------------------------------

} // http_proto
} // boost

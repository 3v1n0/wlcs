/*
 * Copyright © 2017 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 */

#include "in_process_server.h"
#include "display_server.h"
#include "helpers.h"

#include <boost/throw_exception.hpp>
#include <stdexcept>
#include <wayland-client.h>
#include <memory>

class ShimNotImplemented : public std::logic_error
{
public:
    ShimNotImplemented() : std::logic_error("Function not implemented in display server shim")
    {
    }
};

class wlcs::Server::Impl
{
public:
    Impl(int argc, char const** argv)
        : server{wlcs_create_server(argc, argv), &wlcs_destroy_server}
    {
        if (!wlcs_server_start)
        {
            BOOST_THROW_EXCEPTION((std::logic_error{"Missing required wlcs_server_start definition"}));
        }
        if (!wlcs_server_stop)
        {
            BOOST_THROW_EXCEPTION((std::logic_error{"Missing required wlcs_server_stop definition"}));
        }
    }

    void start()
    {
        wlcs_server_start(server.get());
    }

    void stop()
    {
        wlcs_server_stop(server.get());
    }

    int create_client_socket()
    {
        if (wlcs_server_create_client_socket)
        {
            auto fd = wlcs_server_create_client_socket(server.get());
            if (fd < 0)
            {
                BOOST_THROW_EXCEPTION((std::system_error{
                    errno,
                    std::system_category(),
                    "Failed to get client socket from server"}));
            }
            return fd;
        }
        else
        {
            BOOST_THROW_EXCEPTION(ShimNotImplemented{});
        }
    }

private:
    std::unique_ptr<WlcsDisplayServer, void(*)(WlcsDisplayServer*)> const server;
};

wlcs::Server::Server(int argc, char const** argv)
    : impl{std::make_unique<wlcs::Server::Impl>(argc, argv)}
{
}

wlcs::Server::~Server() = default;

void wlcs::Server::start()
{
    impl->start();
}

void wlcs::Server::stop()
{
    impl->stop();
}

int wlcs::Server::create_client_socket()
{
    return impl->create_client_socket();
}

wlcs::InProcessServer::InProcessServer()
    : server{helpers::get_argc(), helpers::get_argv()}
{
}

void wlcs::InProcessServer::SetUp()
{
    server.start();
}

void wlcs::InProcessServer::TearDown()
{
    server.stop();
}

wlcs::Server& wlcs::InProcessServer::the_server()
{
    return server;
}

void throw_wayland_error(wl_display* display)
{
    auto err = wl_display_get_error(display);
    if (err != EPROTO)
    {
        BOOST_THROW_EXCEPTION((std::system_error{
            err,
            std::system_category(),
            "Error while dispatching Wayland events"
        }));
    }
    else
    {
        uint32_t object_id;
        uint32_t protocol_error;
        wl_interface const* interface;
        protocol_error = wl_display_get_protocol_error(display, &interface, &object_id);
        BOOST_THROW_EXCEPTION((wlcs::ProtocolError{interface, protocol_error}));
    }
}

class wlcs::Client::Impl
{
public:
    Impl(Server& server)
    {
        try
        {
            display = wl_display_connect_to_fd(server.create_client_socket());
        }
        catch (ShimNotImplemented const&)
        {
            // TODO: Warn about connecting to who-knows-what
            display = wl_display_connect(NULL);
        }

        if (!display)
        {
            BOOST_THROW_EXCEPTION((std::runtime_error{"Failed to connect to Wayland socket"}));
        }

        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registry_listener, this);

        server_roundtrip();
    }

    ~Impl()
    {
        if (shm) wl_shm_destroy(shm);
        if (shell) wl_shell_destroy(shell);
        if (compositor) wl_compositor_destroy(compositor);
        if (registry) wl_registry_destroy(registry);
        if (shell_surface) wl_shell_surface_destroy(shell_surface);
        wl_display_disconnect(display);
    }

    struct wl_display* wl_display() const
    {
        return display;
    }

    struct wl_compositor* wl_compositor() const
    {
        return compositor;
    }

    struct wl_shm* wl_shm() const
    {
        return shm;
    }

    Surface create_visible_surface(
        Client& client,
        int /*width*/,
        int /*height*/)
    {
        Surface surface{client};

        shell_surface = wl_shell_get_shell_surface(shell, surface);
        wl_shell_surface_set_toplevel(shell_surface);

//        auto buffer = std::make_shared<ShmBuffer>(client, width, height);
//
//        wl_surface_attach(surface, *buffer, 0, 0);
//        wl_surface_commit(surface);
//
//        buffer->add_release_listener([buffer]() { return false; });
//        // It's safe to free the buffer after the release event has been received.
//        buffer.reset();

        return surface;
    }

    void dispatch_until(std::function<bool()> const& predicate)
    {
        // TODO: Drive this with epoll on the fd and have a timerfd for timeout
        while (!predicate())
        {
            if (wl_display_dispatch(display) < 0)
            {
                throw_wayland_error(display);
            }
        }
    }

    void server_roundtrip()
    {
        if (wl_display_roundtrip(display) < 0)
        {
            throw_wayland_error(display);
        }
    }

private:
    static void global_handler(
        void* ctx,
        wl_registry* registry,
        uint32_t id,
        char const* interface,
        uint32_t version)
    {
        using namespace std::literals::string_literals;

        auto me = static_cast<Impl*>(ctx);

        if ("wl_shm"s == interface)
        {
            me->shm = static_cast<struct wl_shm*>(
                wl_registry_bind(registry, id, &wl_shm_interface, version));
        }
        else if ("wl_compositor"s == interface)
        {
            me->compositor = static_cast<struct wl_compositor*>(
                wl_registry_bind(registry, id, &wl_compositor_interface, version));
        }
        else if ("wl_shell"s == interface)
        {
            me->shell = static_cast<struct wl_shell*>(
                wl_registry_bind(registry, id, &wl_shell_interface, version));
        }
    }

    constexpr static wl_registry_listener registry_listener = {
        &global_handler,
        nullptr
    };

    struct wl_display* display;
    struct wl_registry* registry = nullptr;
    struct wl_compositor* compositor = nullptr;
    struct wl_shm* shm = nullptr;
    struct wl_shell_surface* shell_surface = nullptr;
    struct wl_shell* shell = nullptr;
};

constexpr wl_registry_listener wlcs::Client::Impl::registry_listener;

wlcs::Client::Client(Server& server)
    : impl{std::make_unique<Impl>(server)}
{
}

wlcs::Client::~Client() = default;

wlcs::Client::operator wl_display*() const
{
    return impl->wl_display();
}

wl_compositor* wlcs::Client::compositor() const
{
    return impl->wl_compositor();
}

wl_shm* wlcs::Client::shm() const
{
    return impl->wl_shm();
}

wlcs::Surface wlcs::Client::create_visible_surface(int width, int height)
{
    return impl->create_visible_surface(*this, width, height);
}

void wlcs::Client::dispatch_until(std::function<bool()> const& predicate)
{
    impl->dispatch_until(predicate);
}

class wlcs::Surface::Impl
{
public:
    Impl(Client& client)
        : surface_{wl_compositor_create_surface(client.compositor())}
    {
    }

    ~Impl()
    {
        if (pending_callback)
        {
            delete static_cast<std::function<void(uint32_t)>*>(wl_callback_get_user_data(pending_callback));

            wl_callback_destroy(pending_callback);
        }

        wl_surface_destroy(surface_);
    }

    wl_surface* surface() const
    {
        return surface_;
    }

    void add_frame_callback(std::function<void(uint32_t)> const& on_frame)
    {
        std::unique_ptr<std::function<void(uint32_t)>> holder{
            new std::function<void(uint32_t)>(on_frame)};

        pending_callback = wl_surface_frame(surface_);

        // TODO: Store pending callbacks and destroy + free closure on ~Surface
        wl_callback_add_listener(pending_callback, &frame_listener, holder.release());
    }

private:

    static wl_callback* pending_callback;

    static void frame_callback(void* ctx, wl_callback* callback, uint32_t frame_time)
    {
        pending_callback = nullptr;

        auto frame_callback = static_cast<std::function<void(uint32_t)>*>(ctx);

        (*frame_callback)(frame_time);

        wl_callback_destroy(callback);
        delete frame_callback;
    }

    static constexpr wl_callback_listener frame_listener = {
        &frame_callback
    };

    struct wl_surface* const surface_;
};

wl_callback* wlcs::Surface::Impl::pending_callback = nullptr;

constexpr wl_callback_listener wlcs::Surface::Impl::frame_listener;

wlcs::Surface::Surface(Client& client)
    : impl{std::make_unique<Impl>(client)}
{
}

wlcs::Surface::~Surface() = default;

wlcs::Surface::Surface(Surface&&) = default;

wlcs::Surface::operator wl_surface*() const
{
    return impl->surface();
}

void wlcs::Surface::add_frame_callback(std::function<void(int)> const& on_frame)
{
    impl->add_frame_callback(on_frame);
}

class wlcs::ShmBuffer::Impl
{
public:
    Impl(Client& client, int width, int height)
    {
        auto stride = width * 4;
        auto size = stride * height;
        auto fd = wlcs::helpers::create_anonymous_file(size);

        auto pool = wl_shm_create_pool(client.shm(), fd, size);
        buffer_ = wl_shm_pool_create_buffer(
            pool,
            0,
            width,
            height,
            stride,
            WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);

        wl_buffer_add_listener(buffer_, &listener, this);
    }

    ~Impl()
    {
        wl_buffer_destroy(buffer_);
    }

    wl_buffer* buffer() const
    {
        return buffer_;
    }

    void add_release_listener(std::function<bool()> const& on_release)
    {
        release_notifiers.push_back(on_release);
    }

private:
    static void on_release(void* ctx, wl_buffer* /*buffer*/)
    {
        auto me = static_cast<Impl*>(ctx);

        std::vector<decltype(me->release_notifiers.begin())> expired_notifiers;

        for (auto notifier = me->release_notifiers.begin(); notifier != me->release_notifiers.end(); ++notifier)
        {
            if (!(*notifier)())
            {
                expired_notifiers.push_back(notifier);
            }
        }
        for (auto const& expired : expired_notifiers)
            me->release_notifiers.erase(expired);
    }

    static constexpr wl_buffer_listener listener {
        &on_release
    };

    wl_buffer* buffer_;
    std::vector<std::function<bool()>> release_notifiers;
};

constexpr wl_buffer_listener wlcs::ShmBuffer::Impl::listener;

wlcs::ShmBuffer::ShmBuffer(Client &client, int width, int height)
    : impl{std::make_unique<Impl>(client, width, height)}
{
}

wlcs::ShmBuffer::ShmBuffer(ShmBuffer&&) = default;
wlcs::ShmBuffer::~ShmBuffer() = default;

wlcs::ShmBuffer::operator wl_buffer*() const
{
    return impl->buffer();
}

void wlcs::ShmBuffer::add_release_listener(std::function<bool()> const &on_release)
{
    impl->add_release_listener(on_release);
}

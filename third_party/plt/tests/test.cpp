#include "test.h"

#include "fiber.h"

#include <std/ios/input.h>
#include <std/ios/output.h>
#include <std/ptr/scoped.h>
#include <std/thr/runable.h>

#include "cursor-shape-v1-server-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "primary-selection-unstable-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-activation-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#include <std/mem/obj_pool.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/str/view.h>
#include <std/sys/crt.h>
#include <std/thr/poll_fd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

using namespace stl;

#ifdef __LLVM_INSTR_PROFILE_GENERATE
extern "C" int __llvm_profile_dump();
extern "C" void __llvm_profile_reset_counters();
#endif

namespace plt::test {

    bool transfer(int fd, void* data, size_t size, bool writeData) {
        auto* cursor = static_cast<unsigned char*>(data);
        while (size != 0) {
            const ssize_t count = writeData ? write(fd, cursor, size) : read(fd, cursor, size);
            if (count > 0) {
                cursor += count;
                size -= static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    }

    void readOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read) {
        // Leaked per call: the harness spawns a handful per scenario.
        u8* const stack = new u8[lightFiberStack];
        platform.scheduler()->spawn(*stl::makeRunablePtr([&clipboard, &read] {
            const stl::ScopedPtr<stl::Input> stream{clipboard.read()};
            for (;;) {
                u8 chunk[8 * 1024];
                const size_t count = stream->read(chunk, sizeof(chunk));
                if (count == 0) {
                    break;
                }
                read.content.append(chunk, count);
                ++read.chunks;
            }
            read.complete = true;
        }), stack, lightFiberStack);
    }

    void abortOnFiber(Platform& platform, Clipboard& clipboard, StreamRead& read) {
        u8* const stack = new u8[lightFiberStack];
        platform.scheduler()->spawn(*stl::makeRunablePtr([&clipboard, &read] {
            const stl::ScopedPtr<stl::Input> stream{clipboard.read()};
            u8 chunk[8 * 1024];
            const size_t count = stream->read(chunk, sizeof(chunk));
            if (count != 0) {
                read.content.append(chunk, count);
                ++read.chunks;
            }
            read.complete = true;
        }), stack, lightFiberStack);
    }

    void writeClipboard(Clipboard& clipboard, stl::StringView content) {
        const stl::ScopedPtr<stl::Output> output{clipboard.write()};
        output->write(content.data(), content.length());
        output->finish();
    }

    Buffer repeated(size_t size, u8 value) {
        Buffer result(size);
        result.zero(size);
        u8* const bytes = static_cast<u8*>(result.mutData());
        for (size_t index = 0; index != size; ++index) {
            bytes[index] = value;
        }
        return result;
    }

    struct Server;

    struct Surface {
        Server* server = nullptr;
        wl_resource* surface = nullptr;
        wl_resource* xdgSurface = nullptr;
        wl_resource* toplevel = nullptr;
        wl_resource* fractionalScale = nullptr;
        bool configured = false;
    };

    struct Server {
        Server();
        ~Server();

        bool run(int controlFd, pid_t child);
        void handle(Command command, int controlFd);
        bool offerSelection(const char* mime);
        bool dragEnter(const char* mime, const char* extraMime = nullptr);
        void sendInitialConfigure(Surface& surface);
        void sendConfigure(Surface& surface, i32 width, i32 height, const u32* states, size_t stateCount);

        wl_display* display = nullptr;
        wl_event_loop* loop = nullptr;
        wl_client* client = nullptr;
        wl_resource* pointer = nullptr;
        wl_resource* keyboard = nullptr;
        wl_resource* dataDevice = nullptr;
        wl_resource* dataSource = nullptr;
        wl_resource* dataOffer = nullptr;
        wl_resource* primaryDevice = nullptr;
        wl_resource* primarySource = nullptr;
        wl_resource* primaryOffer = nullptr;
        wl_resource* cursorShapeDevice = nullptr;
        wl_resource* textInput = nullptr;
        wl_global* outputGlobal = nullptr;
        wl_global* seatGlobal = nullptr;
        wl_global* cursorShapeGlobal = nullptr;
        u32 selectionSerial = 0;
        Surface* window = nullptr;
        Vector<wl_resource*> frameCallbacks;
        wl_event_source* writeSource = nullptr;
        int readWriteFd = -1;
        int writeReadFd = -1;
        u32 writtenBytes = 0;
        u32 titleCount = 0;
        u32 targetTitleCount = 0;
        u32 serial = 1;
        u32 controlModifier = 0;
        u32 shiftModifier = 0;
        u32 capsLockModifier = 0;
        u32 selectionCount = 0;
        u32 primarySelectionCount = 0;
        u32 requestFlags = 0;
        u32 frameRequestCount = 0;
        u32 cursorShapeCount = 0;
        u32 cursorShape = 0;
        u32 cursorShapeSerial = 0;
        wl_resource* dragOffer = nullptr;
        u32 dragAcceptCount = 0;
        i32 dragAcceptMime = 0;
        u32 dragPreferredAction = 0;
        u32 dragFinishCount = 0;
        i32 receiveMime = 0;
        u32 pointerEnterSerial = 0;
        u32 textInputCommitCount = 0;
        u32 textInputPurpose = 0;
        i32 textInputRectX = 0;
        i32 textInputRectY = 0;
        i32 textInputRectWidth = 0;
        i32 textInputRectHeight = 0;
        bool textInputEnabled = false;
        bool textInputPendingEnabled = false;
        bool textInputPendingDisabled = false;
        u32 activationCount = 0;
        i32 geometryWidth = 0;
        i32 geometryHeight = 0;
        i32 minimumWidth = 0;
        i32 minimumHeight = 0;
        u32 minimumCount = 0;
        bool deferInitialConfigure = false;
    };

    void destroyResource(wl_client*, wl_resource* resource) {
        wl_resource_destroy(resource);
    }

    void bindSimple(wl_client* client, void*, u32 version, u32 id, const wl_interface* interface, const void* implementation, void* data) {
        wl_resource* const resource = wl_resource_create(client, interface, static_cast<int>(version), id);
        wl_resource_set_implementation(resource, implementation, data, nullptr);
    }

    void surfaceDestroy(wl_resource* resource) {
        delete static_cast<Surface*>(wl_resource_get_user_data(resource));
    }

    void surfaceCommit(wl_client*, wl_resource* resource) {
        Surface* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        if (surface->xdgSurface != nullptr && surface->toplevel != nullptr && !surface->configured && !surface->server->deferInitialConfigure) {
            surface->server->sendInitialConfigure(*surface);
        }
    }

    const struct wl_surface_interface surfaceImplementation{
        .destroy = destroyResource,
        .attach = [](wl_client*, wl_resource*, wl_resource*, i32, i32) {},
        .damage = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .frame =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        wl_resource* const callback = wl_resource_create(client, &wl_callback_interface, 1, id);
        surface->server->frameCallbacks.pushBack(callback);
        ++surface->server->frameRequestCount;
    },
        .set_opaque_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_input_region = [](wl_client*, wl_resource*, wl_resource*) {},
        .commit = surfaceCommit,
        .set_buffer_transform = [](wl_client*, wl_resource*, i32) {},
        .set_buffer_scale = [](wl_client*, wl_resource*, i32) {},
        .damage_buffer = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .offset = [](wl_client*, wl_resource*, i32, i32) {},
        .get_release = nullptr,
    };

    const struct wl_region_interface regionImplementation{
        .destroy = destroyResource,
        .add = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
        .subtract = [](wl_client*, wl_resource*, i32, i32, i32, i32) {},
    };

    const struct wl_compositor_interface compositorImplementation{
        .create_surface =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        wl_resource* const surfaceResource = wl_resource_create(client, &wl_surface_interface, min(6, wl_resource_get_version(resource)), id);
        auto* const surface = new Surface{
            .server = server,
            .surface = surfaceResource,
        };
        server->window = surface;
        wl_resource_set_implementation(surfaceResource, &surfaceImplementation, surface, surfaceDestroy);
    },
        .create_region =
            [](wl_client* client, wl_resource* resource, u32 id) {
        wl_resource* const region = wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
        wl_resource_set_implementation(region, &regionImplementation, nullptr, nullptr);
    },
        .release = destroyResource,
    };

    const struct wl_pointer_interface pointerImplementation{
        .set_cursor = [](wl_client*, wl_resource*, u32, wl_resource*, i32, i32) {},
        .release = destroyResource,
    };

    void sendKeymap(Server& server) {
        xkb_context* const context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        xkb_rule_names names{};
        names.layout = "us,ru";
        xkb_keymap* const keymap = context == nullptr ? nullptr : xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
        char* const text = keymap == nullptr ? nullptr : xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        if (keymap != nullptr) {
            const auto modifier = [keymap](const char* name) -> u32 {
                const xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap, name);
                return index != XKB_MOD_INVALID && index < sizeof(u32) * 8 ? 1u << index : 0;
            };
            server.controlModifier = modifier(XKB_MOD_NAME_CTRL);
            server.shiftModifier = modifier(XKB_MOD_NAME_SHIFT);
            server.capsLockModifier = modifier(XKB_MOD_NAME_CAPS);
        }
        if (text != nullptr) {
            const size_t size = strLen(reinterpret_cast<const u8*>(text)) + 1;
            const int fd = memfd_create("plt-keymap", MFD_CLOEXEC);
            if (fd >= 0 && transfer(fd, text, size, true) && lseek(fd, 0, SEEK_SET) == 0) {
                wl_keyboard_send_keymap(server.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<u32>(size));
            }
            if (fd >= 0) {
                close(fd);
            }
            free(text);
        }
        if (keymap != nullptr) {
            xkb_keymap_unref(keymap);
        }
        if (context != nullptr) {
            xkb_context_unref(context);
        }
    }

    void sendInvalidKeymap(Server& server) {
        static constexpr char text[] = "not an xkb keymap";
        const int fd = memfd_create("plt-invalid-keymap", MFD_CLOEXEC);
        if (fd >= 0 && transfer(fd, const_cast<char*>(text), sizeof(text), true) && lseek(fd, 0, SEEK_SET) == 0) {
            wl_keyboard_send_keymap(server.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, sizeof(text));
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    const struct wl_keyboard_interface keyboardImplementation{
        .release = destroyResource,
    };

    const struct wl_seat_interface seatImplementation{
        .get_pointer =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->pointer = wl_resource_create(client, &wl_pointer_interface, min(8, wl_resource_get_version(resource)), id);
        wl_resource_set_implementation(server->pointer, &pointerImplementation, server, nullptr);
    },
        .get_keyboard =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->keyboard = wl_resource_create(client, &wl_keyboard_interface, min(8, wl_resource_get_version(resource)), id);
        wl_resource_set_implementation(server->keyboard, &keyboardImplementation, server, nullptr);
        sendKeymap(*server);
        wl_keyboard_send_repeat_info(server->keyboard, 1000, 1);
    },
        .get_touch = nullptr,
        .release = destroyResource,
    };

    const struct wl_data_source_interface dataSourceImplementation{
        .offer = [](wl_client*, wl_resource*, const char*) {},
        .destroy =
            [](wl_client*, wl_resource* resource) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->dataSource == resource) {
            server->dataSource = nullptr;
        }
        wl_resource_destroy(resource);
    },
        .set_actions = [](wl_client*, wl_resource*, u32) {},
    };

    i32 mimeCode(const char* mime) {
        if (mime == nullptr) {
            return 0;
        }
        const stl::StringView value(mime);
        if (value == stl::StringView(u8"text/plain;charset=utf-8")) {
            return 1;
        }
        if (value == stl::StringView(u8"text/plain")) {
            return 2;
        }
        if (value == stl::StringView(u8"UTF8_STRING")) {
            return 3;
        }
        if (value == stl::StringView(u8"text/uri-list")) {
            return 4;
        }
        return -1;
    }

    const struct wl_data_offer_interface dataOfferImplementation{
        .accept =
            [](wl_client*, wl_resource* resource, u32, const char* mime) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->dragAcceptCount;
        server->dragAcceptMime = mimeCode(mime);
    },
        .receive =
            [](wl_client*, wl_resource* resource, const char* mime, i32 fd) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->readWriteFd != -1) {
            close(server->readWriteFd);
        }
        server->readWriteFd = fd;
        server->receiveMime = mimeCode(mime);
    },
        .destroy =
            [](wl_client*, wl_resource* resource) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->dataOffer == resource) {
            server->dataOffer = nullptr;
        }
        if (server->dragOffer == resource) {
            server->dragOffer = nullptr;
        }
        wl_resource_destroy(resource);
    },
        .finish =
            [](wl_client*, wl_resource* resource) {
        ++static_cast<Server*>(wl_resource_get_user_data(resource))->dragFinishCount;
    },
        .set_actions =
            [](wl_client*, wl_resource* resource, u32, u32 preferred) {
        static_cast<Server*>(wl_resource_get_user_data(resource))->dragPreferredAction = preferred;
    },
    };

    const struct wl_data_device_interface dataDeviceImplementation{
        .start_drag = nullptr,
        .set_selection =
            [](wl_client*, wl_resource* resource, wl_resource*, u32 serial) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->selectionCount;
        server->selectionSerial = serial;
    },
        .release = destroyResource,
    };

    const struct wl_data_device_manager_interface dataManagerImplementation{
        .create_data_source =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        wl_resource* const source = wl_resource_create(client, &wl_data_source_interface, wl_resource_get_version(resource), id);
        server->dataSource = source;
        wl_resource_set_implementation(source, &dataSourceImplementation, server, nullptr);
    },
        .get_data_device =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->dataDevice = wl_resource_create(client, &wl_data_device_interface, wl_resource_get_version(resource), id);
        wl_resource_set_implementation(server->dataDevice, &dataDeviceImplementation, server, nullptr);
    },
        .release = destroyResource,
    };

    const struct zwp_primary_selection_source_v1_interface primarySourceImplementation{
        .offer = [](wl_client*, wl_resource*, const char*) {},
        .destroy = [](wl_client*, wl_resource* resource) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->primarySource == resource) {
            server->primarySource = nullptr;
        }
        wl_resource_destroy(resource);
    },
    };

    const struct zwp_primary_selection_offer_v1_interface primaryOfferImplementation{
        .receive =
            [](wl_client*, wl_resource* resource, const char*, i32 fd) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->readWriteFd != -1) {
            close(server->readWriteFd);
        }
        server->readWriteFd = fd;
    },
        .destroy = [](wl_client*, wl_resource* resource) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        if (server->primaryOffer == resource) {
            server->primaryOffer = nullptr;
        }
        wl_resource_destroy(resource);
    },
    };

    const struct zwp_primary_selection_device_v1_interface primaryDeviceImplementation{
        .set_selection =
            [](wl_client*, wl_resource* resource, wl_resource*, u32) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->primarySelectionCount;
    },
        .destroy = destroyResource,
    };

    const struct zwp_primary_selection_device_manager_v1_interface primaryManagerImplementation{
        .create_source =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->primarySource = wl_resource_create(client, &zwp_primary_selection_source_v1_interface, 1, id);
        wl_resource_set_implementation(server->primarySource, &primarySourceImplementation, server, nullptr);
    },
        .get_device =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->primaryDevice = wl_resource_create(client, &zwp_primary_selection_device_v1_interface, 1, id);
        wl_resource_set_implementation(server->primaryDevice, &primaryDeviceImplementation, server, nullptr);
    },
        .destroy = destroyResource,
    };

    const struct xdg_toplevel_interface toplevelImplementation{
        .destroy = destroyResource,
        .set_parent = [](wl_client*, wl_resource*, wl_resource*) {},
        .set_title =
            [](wl_client*, wl_resource* resource, const char* title) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        Server& server = *surface->server;
        ++server.titleCount;
        if (StringView(title) == StringView(u8"updated title")) {
            server.requestFlags |= UpdatedTitle;
        }
        if (server.targetTitleCount != 0 && server.titleCount >= server.targetTitleCount) {
            xdg_toplevel_send_close(resource);
            wl_display_flush_clients(server.display);
            server.targetTitleCount = 0;
        }
    },
        .set_app_id =
            [](wl_client*, wl_resource* resource, const char* appId) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        if (StringView(appId) == StringView(u8"plt.integration")) {
            surface->server->requestFlags |= InitialAppId;
        }
    },
        .show_window_menu = [](wl_client*, wl_resource*, wl_resource*, u32, i32, i32) {},
        .move =
            [](wl_client*, wl_resource* resource, wl_resource*, u32) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Move;
    },
        .resize = [](wl_client*, wl_resource*, wl_resource*, u32, u32) {},
        .set_max_size = [](wl_client*, wl_resource*, i32, i32) {},
        .set_min_size =
            [](wl_client*, wl_resource* resource, i32 width, i32 height) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->minimumWidth = width;
        surface->server->minimumHeight = height;
        ++surface->server->minimumCount;
    },
        .set_maximized =
            [](wl_client*, wl_resource* resource) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Maximize;
    },
        .unset_maximized =
            [](wl_client*, wl_resource* resource) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Unmaximize;
    },
        .set_fullscreen =
            [](wl_client*, wl_resource* resource, wl_resource*) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Fullscreen;
    },
        .unset_fullscreen =
            [](wl_client*, wl_resource* resource) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Unfullscreen;
    },
        .set_minimized = [](wl_client*, wl_resource* resource) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->requestFlags |= Minimize;
    },
    };

    const struct xdg_surface_interface xdgSurfaceImplementation{
        .destroy = destroyResource,
        .get_toplevel =
            [](wl_client* client, wl_resource* resource, u32 id) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->toplevel = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
        wl_resource_set_implementation(surface->toplevel, &toplevelImplementation, surface, nullptr);
    },
        .get_popup = nullptr,
        .set_window_geometry =
            [](wl_client*, wl_resource* resource, i32, i32, i32 width, i32 height) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        surface->server->geometryWidth = width;
        surface->server->geometryHeight = height;
    },
        .ack_configure = [](wl_client*, wl_resource*, u32) {},
    };

    const struct xdg_wm_base_interface wmBaseImplementation{
        .destroy = destroyResource,
        .create_positioner = nullptr,
        .get_xdg_surface =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource* wlSurface) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(wlSurface));
        surface->xdgSurface = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
        wl_resource_set_implementation(surface->xdgSurface, &xdgSurfaceImplementation, surface, nullptr);
    },
        .pong = [](wl_client*, wl_resource*, u32) {},
    };

    const struct wp_viewport_interface viewportImplementation{
        .destroy = destroyResource,
        .set_source = [](wl_client*, wl_resource*, wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t) {},
        .set_destination = [](wl_client*, wl_resource*, i32, i32) {},
    };

    const struct wp_viewporter_interface viewporterImplementation{
        .destroy = destroyResource,
        .get_viewport = [](wl_client* client, wl_resource*, u32 id, wl_resource*) {
        wl_resource* const viewport = wl_resource_create(client, &wp_viewport_interface, 1, id);
        wl_resource_set_implementation(viewport, &viewportImplementation, nullptr, nullptr);
    },
    };

    const struct wp_fractional_scale_v1_interface fractionalScaleImplementation{
        .destroy = destroyResource,
    };

    const struct wp_fractional_scale_manager_v1_interface fractionalManagerImplementation{
        .destroy = destroyResource,
        .get_fractional_scale = [](wl_client* client, wl_resource*, u32 id, wl_resource* wlSurface) {
        auto* const surface = static_cast<Surface*>(wl_resource_get_user_data(wlSurface));
        surface->fractionalScale = wl_resource_create(client, &wp_fractional_scale_v1_interface, 1, id);
        wl_resource_set_implementation(surface->fractionalScale, &fractionalScaleImplementation, surface, nullptr);
    },
    };

    const struct wp_cursor_shape_device_v1_interface cursorShapeDeviceImplementation{
        .destroy = destroyResource,
        .set_shape = [](wl_client*, wl_resource* resource, u32 serial, u32 shape) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->cursorShapeCount;
        server->cursorShape = shape;
        server->cursorShapeSerial = serial;
    },
    };

    const struct zwp_text_input_v3_interface textInputImplementation{
        .destroy = destroyResource,
        .enable =
            [](wl_client*, wl_resource* resource) {
        static_cast<Server*>(wl_resource_get_user_data(resource))->textInputPendingEnabled = true;
    },
        .disable =
            [](wl_client*, wl_resource* resource) {
        static_cast<Server*>(wl_resource_get_user_data(resource))->textInputPendingDisabled = true;
    },
        .set_surrounding_text = [](wl_client*, wl_resource*, const char*, i32, i32) {},
        .set_text_change_cause = [](wl_client*, wl_resource*, u32) {},
        .set_content_type =
            [](wl_client*, wl_resource* resource, u32, u32 purpose) {
        static_cast<Server*>(wl_resource_get_user_data(resource))->textInputPurpose = purpose;
    },
        .set_cursor_rectangle =
            [](wl_client*, wl_resource* resource, i32 x, i32 y, i32 width, i32 height) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->textInputRectX = x;
        server->textInputRectY = y;
        server->textInputRectWidth = width;
        server->textInputRectHeight = height;
    },
        .commit =
            [](wl_client*, wl_resource* resource) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->textInputCommitCount;
        if (server->textInputPendingEnabled) {
            server->textInputEnabled = true;
        }
        if (server->textInputPendingDisabled) {
            server->textInputEnabled = false;
        }
        server->textInputPendingEnabled = false;
        server->textInputPendingDisabled = false;
    },
        .set_available_actions = [](wl_client*, wl_resource*, wl_array*) {},
        .show_input_panel = [](wl_client*, wl_resource*) {},
        .hide_input_panel = [](wl_client*, wl_resource*) {},
    };

    const struct zwp_text_input_manager_v3_interface textInputManagerImplementation{
        .destroy = destroyResource,
        .get_text_input = [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->textInput = wl_resource_create(client, &zwp_text_input_v3_interface, 1, id);
        wl_resource_set_implementation(server->textInput, &textInputImplementation, server, nullptr);
    },
    };

    const struct wp_cursor_shape_manager_v1_interface cursorShapeManagerImplementation{
        .destroy = destroyResource,
        .get_pointer =
            [](wl_client* client, wl_resource* resource, u32 id, wl_resource*) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        server->cursorShapeDevice = wl_resource_create(client, &wp_cursor_shape_device_v1_interface, wl_resource_get_version(resource), id);
        wl_resource_set_implementation(server->cursorShapeDevice, &cursorShapeDeviceImplementation, server, nullptr);
    },
        .get_tablet_tool_v2 = nullptr,
    };

    const struct xdg_activation_token_v1_interface activationTokenImplementation{
        .set_serial = [](wl_client*, wl_resource*, u32, wl_resource*) {},
        .set_app_id = [](wl_client*, wl_resource*, const char*) {},
        .set_surface = [](wl_client*, wl_resource*, wl_resource*) {},
        .commit =
            [](wl_client*, wl_resource* resource) {
        xdg_activation_token_v1_send_done(resource, "plt-test-token");
    },
        .destroy = destroyResource,
    };

    const struct xdg_activation_v1_interface activationImplementation{
        .destroy = destroyResource,
        .get_activation_token =
            [](wl_client* client, wl_resource* resource, u32 id) {
        wl_resource* const token = wl_resource_create(client, &xdg_activation_token_v1_interface, 1, id);
        wl_resource_set_implementation(token, &activationTokenImplementation, wl_resource_get_user_data(resource), nullptr);
    },
        .activate = [](wl_client*, wl_resource* resource, const char*, wl_resource*) {
        auto* const server = static_cast<Server*>(wl_resource_get_user_data(resource));
        ++server->activationCount;
    },
    };

    const struct zxdg_toplevel_decoration_v1_interface decorationImplementation{
        .destroy = destroyResource,
        .set_mode = [](wl_client*, wl_resource*, u32) {},
        .unset_mode = [](wl_client*, wl_resource*) {},
    };

    const struct zxdg_decoration_manager_v1_interface decorationManagerImplementation{
        .destroy = destroyResource,
        .get_toplevel_decoration = [](wl_client* client, wl_resource*, u32 id, wl_resource*) {
        wl_resource* const decoration = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, 1, id);
        wl_resource_set_implementation(decoration, &decorationImplementation, nullptr, nullptr);
    },
    };

    void bindCompositor(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &wl_compositor_interface, &compositorImplementation, data);
    }

    void bindSeat(wl_client* client, void* data, u32 version, u32 id) {
        wl_resource* const resource = wl_resource_create(client, &wl_seat_interface, version, id);
        wl_resource_set_implementation(resource, &seatImplementation, data, nullptr);
        wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
        if (version >= WL_SEAT_NAME_SINCE_VERSION) {
            wl_seat_send_name(resource, "plt-test-seat");
        }
    }

    void bindDataManager(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &wl_data_device_manager_interface, &dataManagerImplementation, data);
    }

    void bindWmBase(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &xdg_wm_base_interface, &wmBaseImplementation, data);
    }

    void bindViewporter(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &wp_viewporter_interface, &viewporterImplementation, data);
    }

    void bindFractional(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &wp_fractional_scale_manager_v1_interface, &fractionalManagerImplementation, data);
    }

    void bindPrimary(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &zwp_primary_selection_device_manager_v1_interface, &primaryManagerImplementation, data);
    }

    void bindCursorShape(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &wp_cursor_shape_manager_v1_interface, &cursorShapeManagerImplementation, data);
    }

    void bindTextInputManager(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &zwp_text_input_manager_v3_interface, &textInputManagerImplementation, data);
    }

    void bindActivation(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &xdg_activation_v1_interface, &activationImplementation, data);
    }

    void bindDecoration(wl_client* client, void* data, u32 version, u32 id) {
        bindSimple(client, nullptr, version, id, &zxdg_decoration_manager_v1_interface, &decorationManagerImplementation, data);
    }

    void bindOutput(wl_client* client, void*, u32 version, u32 id) {
        static const struct wl_output_interface outputImplementation{
            .release = destroyResource,
        };
        wl_resource* const resource = wl_resource_create(client, &wl_output_interface, version, id);
        wl_resource_set_implementation(resource, &outputImplementation, nullptr, nullptr);
        wl_output_send_geometry(resource, 0, 0, 300, 200, WL_OUTPUT_SUBPIXEL_UNKNOWN, "plt", "test", WL_OUTPUT_TRANSFORM_NORMAL);
        wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
        if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
            wl_output_send_scale(resource, 1);
        }
        if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
            wl_output_send_done(resource);
        }
    }

    Server::Server() {
        display = wl_display_create();
        loop = wl_display_get_event_loop(display);
        wl_global_create(display, &wl_compositor_interface, 6, this, bindCompositor);
        seatGlobal = wl_global_create(display, &wl_seat_interface, 8, this, bindSeat);
        wl_global_create(display, &wl_data_device_manager_interface, 3, this, bindDataManager);
        wl_global_create(display, &xdg_wm_base_interface, 6, this, bindWmBase);
        wl_global_create(display, &wp_viewporter_interface, 1, this, bindViewporter);
        wl_global_create(display, &wp_fractional_scale_manager_v1_interface, 1, this, bindFractional);
        wl_global_create(display, &zwp_primary_selection_device_manager_v1_interface, 1, this, bindPrimary);
        cursorShapeGlobal = wl_global_create(display, &wp_cursor_shape_manager_v1_interface, 2, this, bindCursorShape);
        wl_global_create(display, &xdg_activation_v1_interface, 1, this, bindActivation);
        wl_global_create(display, &zxdg_decoration_manager_v1_interface, 1, this, bindDecoration);
        wl_global_create(display, &zwp_text_input_manager_v3_interface, 1, this, bindTextInputManager);
        outputGlobal = wl_global_create(display, &wl_output_interface, 4, this, bindOutput);
    }

    Server::~Server() {
        if (writeSource != nullptr) {
            wl_event_source_remove(writeSource);
        }
        if (readWriteFd != -1) {
            close(readWriteFd);
        }
        if (writeReadFd != -1) {
            close(writeReadFd);
        }
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
    }

    void Server::sendInitialConfigure(Surface& surface) {
        sendConfigure(surface, 0, 0, nullptr, 0);
        surface.configured = true;
    }

    void Server::sendConfigure(Surface& surface, i32 width, i32 height, const u32* states, size_t stateCount) {
        wl_array stateArray;
        wl_array_init(&stateArray);
        for (size_t index = 0; index != stateCount; ++index) {
            auto* const target = static_cast<u32*>(wl_array_add(&stateArray, sizeof(u32)));
            if (target != nullptr) {
                *target = states[index];
            }
        }
        xdg_toplevel_send_configure(surface.toplevel, width, height, &stateArray);
        xdg_surface_send_configure(surface.xdgSurface, serial++);
        wl_array_release(&stateArray);
        wl_display_flush_clients(display);
    }

    bool Server::offerSelection(const char* mime) {
        if (dataDevice == nullptr) {
            return false;
        }
        dataOffer = wl_resource_create(client, &wl_data_offer_interface, wl_resource_get_version(dataDevice), 0);
        wl_resource_set_implementation(dataOffer, &dataOfferImplementation, this, nullptr);
        wl_data_device_send_data_offer(dataDevice, dataOffer);
        wl_data_offer_send_offer(dataOffer, mime);
        wl_data_device_send_selection(dataDevice, dataOffer);
        wl_display_flush_clients(display);
        return true;
    }

    bool Server::dragEnter(const char* mime, const char* extraMime) {
        if (dataDevice == nullptr || window == nullptr) {
            return false;
        }
        dragOffer = wl_resource_create(client, &wl_data_offer_interface, wl_resource_get_version(dataDevice), 0);
        wl_resource_set_implementation(dragOffer, &dataOfferImplementation, this, nullptr);
        wl_data_device_send_data_offer(dataDevice, dragOffer);
        wl_data_offer_send_offer(dragOffer, mime);
        if (extraMime != nullptr) {
            wl_data_offer_send_offer(dragOffer, extraMime);
        }
        wl_data_offer_send_source_actions(dragOffer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        wl_data_device_send_enter(dataDevice, serial++, window->surface, wl_fixed_from_int(15), wl_fixed_from_int(25), dragOffer);
        wl_display_flush_clients(display);
        return true;
    }

    void Server::handle(Command command, int controlFd) {
        Reply reply;
        switch (command) {
            case Command::DeferInitialConfigure:
                deferInitialConfigure = true;
                reply.count = 1;
                break;
            case Command::ReleaseInitialConfigure:
                deferInitialConfigure = false;
                if (window != nullptr && !window->configured) {
                    sendInitialConfigure(*window);
                    reply.count = 1;
                }
                break;
            case Command::QueryInitialConfigure:
                reply.count = window != nullptr && window->configured;
                break;
            case Command::PointerEnter:
                reply.count = selectionCount;
                if (pointer != nullptr && window != nullptr) {
                    pointerEnterSerial = serial;
                    wl_pointer_send_enter(pointer, serial++, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
                    wl_display_flush_clients(display);
                }
                break;
            case Command::PreferredScale:
                if (window != nullptr && window->fractionalScale != nullptr) {
                    wp_fractional_scale_v1_send_preferred_scale(window->fractionalScale, 150);
                    wl_display_flush_clients(display);
                }
                break;
            case Command::QuerySelection:
                reply.count = selectionCount;
                break;
            case Command::QueryMinimum:
                reply.count = minimumCount;
                reply.first = minimumWidth;
                reply.second = minimumHeight;
                break;
            case Command::OfferSelection:
                reply.count = offerSelection("text/plain;charset=utf-8");
                break;
            case Command::OfferPlainSelection:
                reply.count = offerSelection("text/plain");
                break;
            case Command::OfferUnsupportedSelection:
                reply.count = offerSelection("application/octet-stream");
                break;
            case Command::ReleaseRead:
                reply.count = readWriteFd != -1;
                if (readWriteFd != -1) {
                    static constexpr char content[] = "hermetic Wayland clipboard";
                    transfer(readWriteFd, const_cast<char*>(content), sizeof(content) - 1, true);
                    close(readWriteFd);
                    readWriteFd = -1;
                }
                break;
            case Command::RequestSourceData:
                if (dataSource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        writeReadFd = pipes[0];
                        wl_data_source_send_send(dataSource, "text/plain;charset=utf-8", pipes[1]);
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::RequestBrokenSourceData:
                if (dataSource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        close(pipes[0]);
                        wl_data_source_send_send(dataSource, "text/plain;charset=utf-8", pipes[1]);
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::CancelSources:
                if (dataSource != nullptr) {
                    wl_data_source_send_cancelled(dataSource);
                    reply.count |= 1;
                }
                if (primarySource != nullptr) {
                    zwp_primary_selection_source_v1_send_cancelled(primarySource);
                    reply.count |= 2;
                }
                wl_display_flush_clients(display);
                break;
            case Command::ReleaseWrite:
                if (writeReadFd != -1 && writeSource == nullptr) {
                    fcntl(writeReadFd, F_SETFL, fcntl(writeReadFd, F_GETFL) | O_NONBLOCK);
                    writeSource = wl_event_loop_add_fd(loop, writeReadFd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR, [](int fd, u32 mask, void* data) {
                        auto* const server = static_cast<Server*>(data);
                        char buffer[16384];
                        for (;;) {
                            const ssize_t count = read(fd, buffer, sizeof(buffer));
                            if (count > 0) {
                                server->writtenBytes += static_cast<u32>(count);
                            } else if (count < 0 && errno == EINTR) {
                                continue;
                            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                break;
                            } else {
                                wl_event_source_remove(server->writeSource);
                                server->writeSource = nullptr;
                                close(server->writeReadFd);
                                server->writeReadFd = -1;
                                break;
                            }
                        }
                        if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR) && server->writeSource != nullptr) {
                            wl_event_source_remove(server->writeSource);
                            server->writeSource = nullptr;
                            close(server->writeReadFd);
                            server->writeReadFd = -1;
                        }
                        return 0;
                    }, this);
                    reply.count = 1;
                }
                break;
            case Command::QueryWrite:
                reply.count = writtenBytes;
                reply.first = writeReadFd == -1;
                break;
            case Command::AwaitTitles:
                targetTitleCount = static_cast<u32>(reply.first = 2049);
                reply.count = titleCount;
                if (window != nullptr && window->toplevel != nullptr && titleCount >= targetTitleCount) {
                    xdg_toplevel_send_close(window->toplevel);
                    wl_display_flush_clients(display);
                    targetTitleCount = 0;
                }
                break;
            case Command::ConfigureWindowState:
                if (window != nullptr && window->toplevel != nullptr) {
                    const u32 states[]{
                        XDG_TOPLEVEL_STATE_ACTIVATED,
                        XDG_TOPLEVEL_STATE_MAXIMIZED,
                        XDG_TOPLEVEL_STATE_FULLSCREEN,
                        XDG_TOPLEVEL_STATE_TILED_LEFT,
                    };
                    sendConfigure(*window, 900, 700, states, sizeof(states) / sizeof(states[0]));
                    reply.count = 1;
                }
                break;
            case Command::ConfigureWindowResize:
                if (window != nullptr && window->toplevel != nullptr) {
                    sendConfigure(*window, 819, 638, nullptr, 0);
                    reply.count = 1;
                }
                break;
            case Command::CloseWindow:
                if (window != nullptr && window->toplevel != nullptr) {
                    xdg_toplevel_send_close(window->toplevel);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryWindowRequests:
                reply.count = requestFlags;
                break;
            case Command::QueryWindowGeometry:
                reply.first = geometryWidth;
                reply.second = geometryHeight;
                break;
            case Command::QueryFrames:
                reply.count = frameRequestCount;
                reply.first = static_cast<i32>(frameCallbacks.length());
                break;
            case Command::CompleteFrames:
                reply.count = static_cast<u32>(frameCallbacks.length());
                for (wl_resource* callback : frameCallbacks) {
                    wl_callback_send_done(callback, 1);
                    wl_resource_destroy(callback);
                }
                frameCallbacks.clear();
                wl_display_flush_clients(display);
                break;
            case Command::PointerSequence:
                if (pointer != nullptr && window != nullptr) {
                    pointerEnterSerial = serial;
                    wl_pointer_send_enter(pointer, serial++, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
                    wl_pointer_send_motion(pointer, 1000, wl_fixed_from_int(30), wl_fixed_from_int(40));
                    wl_pointer_send_button(pointer, serial++, 1500, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
                    wl_pointer_send_axis(pointer, 1600, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_int(20));
                    wl_pointer_send_axis(pointer, 1600, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-30));
                    wl_pointer_send_frame(pointer);
                    wl_pointer_send_button(pointer, serial++, 1700, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
                    wl_pointer_send_frame(pointer);
                    wl_pointer_send_leave(pointer, serial++, window->surface);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryCursor:
                reply.count = cursorShapeCount;
                reply.first = static_cast<i32>(cursorShape);
                reply.second = pointerEnterSerial != 0 && cursorShapeSerial == pointerEnterSerial;
                break;
            case Command::KeyboardEnter:
                if (keyboard != nullptr && window != nullptr) {
                    wl_array keys;
                    wl_array_init(&keys);
                    wl_keyboard_send_enter(keyboard, serial++, window->surface, &keys);
                    wl_keyboard_send_modifiers(keyboard, serial++, 0, 0, 0, 0);
                    wl_array_release(&keys);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardPress:
                if (keyboard != nullptr) {
                    wl_keyboard_send_key(keyboard, serial++, 2500, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardRelease:
                if (keyboard != nullptr) {
                    wl_keyboard_send_key(keyboard, serial++, 2600, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardControl:
                if (keyboard != nullptr) {
                    wl_keyboard_send_modifiers(keyboard, serial++, controlModifier, 0, 0, 0);
                    wl_display_flush_clients(display);
                    reply.count = controlModifier != 0;
                }
                break;
            case Command::KeyboardControlShift:
                if (keyboard != nullptr) {
                    wl_keyboard_send_modifiers(keyboard, serial++, controlModifier | shiftModifier, 0, 0, 0);
                    wl_display_flush_clients(display);
                    reply.count = controlModifier != 0 && shiftModifier != 0;
                }
                break;
            case Command::KeyboardControlCapsLock:
                if (keyboard != nullptr) {
                    wl_keyboard_send_modifiers(keyboard, serial++, controlModifier, 0, capsLockModifier, 0);
                    wl_display_flush_clients(display);
                    reply.count = controlModifier != 0 && capsLockModifier != 0;
                }
                break;
            case Command::KeyboardRussianControl:
                if (keyboard != nullptr) {
                    wl_keyboard_send_modifiers(keyboard, serial++, controlModifier, 0, 0, 1);
                    wl_display_flush_clients(display);
                    reply.count = controlModifier != 0;
                }
                break;
            case Command::KeyboardLeave:
                if (keyboard != nullptr && window != nullptr) {
                    wl_keyboard_send_leave(keyboard, serial++, window->surface);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::InvalidKeymap:
                if (keyboard != nullptr) {
                    sendInvalidKeymap(*this);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::QueryActivation:
                reply.count = activationCount;
                break;
            case Command::QueryPrimarySelection:
                reply.count = primarySelectionCount;
                break;
            case Command::OfferPrimarySelection:
                if (primaryDevice != nullptr) {
                    primaryOffer = wl_resource_create(client, &zwp_primary_selection_offer_v1_interface, 1, 0);
                    wl_resource_set_implementation(primaryOffer, &primaryOfferImplementation, this, nullptr);
                    zwp_primary_selection_device_v1_send_data_offer(primaryDevice, primaryOffer);
                    zwp_primary_selection_offer_v1_send_offer(primaryOffer, "text/plain;charset=utf-8");
                    zwp_primary_selection_device_v1_send_selection(primaryDevice, primaryOffer);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::RequestPrimarySourceData:
                if (primarySource != nullptr) {
                    int pipes[2];
                    if (pipe(pipes) == 0) {
                        writeReadFd = pipes[0];
                        zwp_primary_selection_source_v1_send_send(primarySource, "text/plain;charset=utf-8", pipes[1]);
                        close(pipes[1]);
                        wl_display_flush_clients(display);
                        reply.count = 1;
                    }
                }
                break;
            case Command::PointerValue120:
                if (pointer != nullptr && window != nullptr) {
                    pointerEnterSerial = serial;
                    wl_pointer_send_enter(pointer, serial++, window->surface, wl_fixed_from_int(10), wl_fixed_from_int(20));
                    wl_pointer_send_frame(pointer);
                    wl_pointer_send_axis(pointer, 1600, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(15));
                    wl_pointer_send_axis_value120(pointer, WL_POINTER_AXIS_VERTICAL_SCROLL, -240);
                    wl_pointer_send_frame(pointer);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::KeyboardEnterWithKeys:
                if (keyboard != nullptr && window != nullptr) {
                    wl_array keys;
                    wl_array_init(&keys);
                    auto* const key = static_cast<u32*>(wl_array_add(&keys, sizeof(u32)));
                    if (key != nullptr) {
                        *key = KEY_A;
                    }
                    wl_keyboard_send_enter(keyboard, serial++, window->surface, &keys);
                    wl_keyboard_send_modifiers(keyboard, serial++, 0, 0, 0, 0);
                    wl_array_release(&keys);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::RemoveOutput:
                if (outputGlobal != nullptr) {
                    wl_global_destroy(outputGlobal);
                    outputGlobal = nullptr;
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::RestoreOutput:
                if (outputGlobal == nullptr) {
                    outputGlobal = wl_global_create(display, &wl_output_interface, 4, this, bindOutput);
                    wl_display_flush_clients(display);
                    reply.count = outputGlobal != nullptr;
                }
                break;
            case Command::TextInputEnter:
                if (textInput != nullptr && window != nullptr) {
                    zwp_text_input_v3_send_enter(textInput, window->surface);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::TextInputPreedit:
                if (textInput != nullptr) {
                    zwp_text_input_v3_send_preedit_string(textInput, "ni", 0, 2);
                    zwp_text_input_v3_send_done(textInput, textInputCommitCount);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::TextInputCommitString:
                if (textInput != nullptr) {
                    zwp_text_input_v3_send_commit_string(textInput, "\xc3\xa9");
                    zwp_text_input_v3_send_done(textInput, textInputCommitCount);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::TextInputCommitInvalid:
                if (textInput != nullptr) {
                    // Overlong '/', a UTF-16 surrogate, and an F5-lead
                    // sequence past U+10FFFF, then one valid scalar.
                    zwp_text_input_v3_send_commit_string(textInput, "\xc0\xaf\xed\xa0\x80\xf5\x8f\xbf\xbf" "A");
                    zwp_text_input_v3_send_done(textInput, textInputCommitCount);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::RemoveSeat:
                if (seatGlobal != nullptr) {
                    wl_global_destroy(seatGlobal);
                    seatGlobal = nullptr;
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::DragEnter:
                reply.count = dragEnter("text/plain;charset=utf-8");
                break;
            case Command::DragEnterUtf8String:
                reply.count = dragEnter("UTF8_STRING");
                break;
            case Command::DragEnterUriList:
                reply.count = dragEnter("text/uri-list", "text/plain;charset=utf-8");
                break;
            case Command::DragMotion:
                if (dataDevice != nullptr) {
                    wl_data_device_send_motion(dataDevice, 0, wl_fixed_from_int(30), wl_fixed_from_int(35));
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::DragDrop:
                if (dataDevice != nullptr) {
                    wl_data_device_send_drop(dataDevice);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::DragLeave:
                if (dataDevice != nullptr) {
                    wl_data_device_send_leave(dataDevice);
                    wl_display_flush_clients(display);
                    reply.count = 1;
                }
                break;
            case Command::DragData:
                reply.count = readWriteFd != -1;
                if (readWriteFd != -1) {
                    static constexpr char content[] = "hermetic Wayland drop";
                    transfer(readWriteFd, const_cast<char*>(content), sizeof(content) - 1, true);
                    close(readWriteFd);
                    readWriteFd = -1;
                }
                break;
            case Command::DragUriData:
                reply.count = readWriteFd != -1;
                if (readWriteFd != -1) {
                    static constexpr char content[] = "file:///tmp/plt%20drop.txt\r\n# comment\r\nhttps://example.com/plt\r\n";
                    transfer(readWriteFd, const_cast<char*>(content), sizeof(content) - 1, true);
                    close(readWriteFd);
                    readWriteFd = -1;
                }
                break;
            case Command::QueryDragAccept:
                reply.count = dragAcceptCount;
                reply.first = dragAcceptMime;
                reply.second = static_cast<i32>(dragPreferredAction);
                break;
            case Command::QueryDragFinish:
                reply.count = dragFinishCount;
                reply.first = receiveMime;
                break;
            case Command::CursorShapeV1:
                if (cursorShapeGlobal != nullptr) {
                    wl_global_destroy(cursorShapeGlobal);
                    cursorShapeGlobal = wl_global_create(display, &wp_cursor_shape_manager_v1_interface, 1, this, bindCursorShape);
                    wl_display_flush_clients(display);
                    reply.count = cursorShapeGlobal != nullptr;
                }
                break;
            case Command::QuerySelectionSerial:
                reply.count = selectionSerial;
                reply.first = static_cast<i32>(serial);
                break;
            case Command::QueryTextInput:
                reply.count = textInputCommitCount;
                reply.first = textInputEnabled ? 1 : 0;
                reply.second = static_cast<i32>(textInputPurpose);
                break;
            case Command::QueryTextInputRect:
                reply.count = static_cast<u32>((textInputRectWidth << 16) | (textInputRectHeight & 0xffff));
                reply.first = textInputRectX;
                reply.second = textInputRectY;
                break;
            case Command::Quit:
                break;
        }
        transfer(controlFd, &reply, sizeof(reply), true);
    }

    bool Server::run(int controlFd, pid_t child) {
        bool controlOpen = true;
        while (controlOpen) {
            pollfd fds[]{
                {
                    .fd = wl_event_loop_get_fd(loop),
                    .events = POLLIN,
                    .revents = 0,
                },
                {
                    .fd = controlFd,
                    .events = POLLIN,
                    .revents = 0,
                },
            };
            int result;
            do {
                result = poll(fds, 2, 5000);
            } while (result < 0 && errno == EINTR);
            if (result <= 0) {
                kill(child, SIGKILL);
                break;
            }
            if (fds[0].revents != 0) {
                if (wl_event_loop_dispatch(loop, 0) < 0) {
                    break;
                }
                wl_display_flush_clients(display);
            }
            if (fds[1].revents & POLLIN) {
                Command command;
                if (!transfer(controlFd, &command, sizeof(command), false)) {
                    controlOpen = false;
                    continue;
                }
                wl_event_loop_dispatch(loop, 0);
                handle(command, controlFd);
                if (command == Command::Quit) {
                    controlOpen = false;
                }
            }
            if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                controlOpen = false;
            }
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    Reply command(int fd, Command commandValue) {
        Reply reply;
        transfer(fd, &commandValue, sizeof(commandValue), true);
        transfer(fd, &reply, sizeof(reply), false);
        return reply;
    }

    struct StopTimer final: plt::TimerCallback {
        explicit StopTimer(plt::Platform& platform_)
            : platform(platform_)
        {
        }

        void ready() override {
            platform.stop();
        }

        plt::Platform& platform;
    };

    void pump(plt::Platform& platform) {
        StopTimer stop(platform);
        platform.poller()->timeout(1000, stop);
        platform.run();
    }

    Client::Client(int controlFd_, u32 width, u32 minimum, plt::WindowEvents* events, plt::InputSink* input, bool waitForConfigure, plt::FrameCallback* frame, plt::DropTarget* drop)
        : controlFd(controlFd_)
        , owner(stl::ObjPool::fromMemory())
    {
        platform = plt::Platform::create(*owner);
        window = platform->createWindow(
            *owner,
            {
                .appId = stl::StringView(u8"plt.integration"),
                .title = stl::StringView(u8"plt integration"),
                .width = width,
                .height = 600,
                .minimumWidth = minimum,
                .minimumHeight = minimum,
                .input = input,
                .events = events,
                .frame = frame,
                .drop = drop,
            }
        );
        window->requestShow();
        if (waitForConfigure) {
            for (u32 attempt = 0; attempt != 10; ++attempt) {
                pump(*platform);
                if (command(controlFd, Command::QueryInitialConfigure).count != 0) {
                    break;
                }
            }
            pump(*platform);
        }
    }

    using Scenario = bool (*)(int);

    bool runScenario(const char* name, Scenario scenario) {
        Server server;
        int wayland[2];
        int control[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland) != 0 || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) != 0) {
            perror("socketpair");
            return false;
        }
        const pid_t child = fork();
        if (child < 0) {
            perror("fork");
            return false;
        }
        if (child == 0) {
#ifdef __LLVM_INSTR_PROFILE_GENERATE
            __llvm_profile_reset_counters();
#endif
            signal(SIGPIPE, SIG_DFL);
            close(wayland[0]);
            close(control[0]);
            char fd[32];
            snprintf(fd, sizeof(fd), "%d", wayland[1]);
            setenv("WAYLAND_SOCKET", fd, 1);
            const bool success = scenario(control[1]);
            command(control[1], Command::Quit);
#ifdef __LLVM_INSTR_PROFILE_GENERATE
            // _exit bypasses the profiling runtime's atexit hook.
            if (__llvm_profile_dump() != 0) {
                fprintf(stderr, "%s: failed to write coverage profile\n", name);
                _exit(1);
            }
#endif
            _exit(success ? 0 : 1);
        }

        close(wayland[1]);
        close(control[1]);
        signal(SIGPIPE, SIG_IGN);
        server.client = wl_client_create(server.display, wayland[0]);
        const bool success = server.run(control[0], child);
        close(control[0]);
        fprintf(stderr, "%s: %s\n", name, success ? "PASS" : "FAIL");
        return success;
    }
}

int main() {
    using namespace plt::test;

    bool success = true;
    success = runScenario("nonblocking show", nonblockingShow) && success;
    success = runScenario("window API", windowApi) && success;
    success = runScenario("multiple windows", multipleWindows) && success;
    success = runScenario("frame API", frameApi) && success;
    success = runScenario("frame retry", frameRetry) && success;
    success = runScenario("pointer input", pointerInput) && success;
    success = runScenario("cursor shapes", cursorShapes) && success;
    success = runScenario("cursor shapes v1", cursorShapesV1) && success;
    success = runScenario("keyboard input", keyboardInput) && success;
    success = runScenario("local selections", localSelections) && success;
    success = runScenario("missing selections", missingSelections) && success;
    success = runScenario("rejected selection", rejectedSelection) && success;
    success = runScenario("plain MIME selection", plainMimeSelection) && success;
    success = runScenario("unsupported MIME selection", unsupportedMimeSelection) && success;
    success = runScenario("selection source cancellation", sourceCancellation) && success;
    success = runScenario("poller API", pollerApi) && success;
    success = runScenario("deferred clipboard", deferredClipboard) && success;
    success = runScenario("fractional rounding", fractionalRounding) && success;
    success = runScenario("minimum after scale", minimumAfterScale) && success;
    success = runScenario("asynchronous clipboard read", asynchronousRead) && success;
    success = runScenario("asynchronous primary selection", asynchronousPrimary) && success;
    success = runScenario("cancel asynchronous clipboard read", cancelAsynchronousRead) && success;
    success = runScenario("cancel ready clipboard read", cancelReadyClipboardRead) && success;
    success = runScenario("fiber clipboard", fiberClipboard) && success;
    success = runScenario("text drop", textDrop) && success;
    success = runScenario("UTF8_STRING drop", utf8StringDrop) && success;
    success = runScenario("uri-list drop", uriListDrop) && success;
    success = runScenario("raw drop API", rawDropApi) && success;
    success = runScenario("rejected drag", rejectedDrag) && success;
    success = runScenario("cancelled drag", cancelledDrag) && success;
    success = runScenario("asynchronous clipboard write", asynchronousWrite) && success;
    success = runScenario("broken clipboard consumer", brokenClipboardConsumer) && success;
    success = runScenario("Wayland flush backpressure", flushBackpressure) && success;
    success = runScenario("queued Wayland event", queuedWaylandEvent) && success;
    success = runScenario("invalid keymap", invalidKeymap) && success;
    success = runScenario("value120 scroll", scrollValue120) && success;
    success = runScenario("keyboard enter pressed keys", keyboardEnterKeys) && success;
    success = runScenario("output removal", outputRemoval) && success;
    success = runScenario("text input", textInput) && success;
    return success ? 0 : 1;
}

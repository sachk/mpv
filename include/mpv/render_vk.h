/* Copyright (C) 2026 the mpv developers
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef MPV_CLIENT_API_RENDER_VK_H_
#define MPV_CLIENT_API_RENDER_VK_H_

#include <stdint.h>

#include "render.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vulkan backend (fork extension)
 * -------------------------------
 *
 * Renders into a VkImage the API user owns, on a VkDevice the API user
 * created. There is no swapchain here and mpv never presents: the caller is
 * expected to be compositing the result into a window of its own, which is
 * what the OpenGL backend's FBO is for and what this replaces on a Vulkan
 * renderer.
 *
 * The device is imported rather than created, so both sides share one device,
 * one queue and one memory allocator. That is what makes the video frame
 * usable by the caller's renderer with no copy and no cross-device sharing.
 *
 * Requirements on the device
 * --------------------------
 *
 * The VkInstance must have been created with an apiVersion of at least the
 * one libplacebo requires, and the VkDevice must have been created with the
 * features libplacebo requires (see pl_vulkan_required_features) and
 * preferably those it recommends. Creating the device yourself and handing it
 * to your UI toolkit, rather than the other way round, is usually the way to
 * guarantee that.
 *
 * Threading and submission
 * ------------------------
 *
 * mpv submits its own command buffers to the first queue of the family named
 * at init. It does not lock that queue, so the caller must not submit to it
 * from another thread while mpv_render_context_render() is running. Calling it
 * from the thread that drives the caller's own renderer, between its
 * submissions, is the intended use -- and being the same queue is also what
 * makes the finished frame visible to the caller's next submission.
 *
 * Image ownership
 * ---------------
 *
 * Each mpv_render_context_render() call takes the image, renders into it and
 * gives it back. `layout` says which layout the image is in when mpv receives
 * it, and mpv leaves it in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ready to
 * be sampled. Ordering is left to the queue: work submitted afterwards on the
 * same queue sees a finished frame. A caller that needs to synchronise across
 * queues has to add its own semaphore around the call.
 */

/**
 * Initialisation parameters for MPV_RENDER_API_TYPE_VULKAN.
 *
 * Handles arrive as void pointers and integers rather than as Vulkan types,
 * so that this header does not drag vulkan.h into every API user. Cast the
 * real handles in.
 */
typedef struct mpv_vulkan_init_params {
    /**
     * vkGetInstanceProcAddr, as PFN_vkGetInstanceProcAddr. Required unless
     * mpv is linked directly against a Vulkan loader that already has it.
     */
    void *get_instance_proc_addr;
    /**
     * VkInstance, VkPhysicalDevice and VkDevice. All required.
     */
    void *instance;
    void *physical_device;
    void *device;
    /**
     * The graphics queue family mpv submits on. mpv uses that family's first
     * queue, and the caller must be using the same one: ordering between the
     * two is what makes the frame safe to sample without a semaphore, and
     * ordering only holds within one queue.
     */
    uint32_t queue_family_index;
    /**
     * The device-level extensions the device was created with, so that mpv
     * only uses what is actually there. May be NULL.
     */
    const char *const *device_extensions;
    int num_device_extensions;
    /**
     * The VkPhysicalDeviceFeatures2 chain the device was created with, as
     * a const VkPhysicalDeviceFeatures2*. May be NULL, in which case mpv
     * assumes only what it strictly requires was enabled.
     */
    const void *device_features;
} mpv_vulkan_init_params;

/**
 * The image to render into, for MPV_RENDER_PARAM_VULKAN_IMAGE.
 */
typedef struct mpv_vulkan_image {
    /**
     * VkImage. Must have been created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
     * and VK_IMAGE_USAGE_SAMPLED_BIT at least, and must be usable by the queue
     * family named at init.
     */
    uint64_t image;
    /**
     * VkFormat of the image. A format with more than 8 bits per component is
     * what makes an HDR target possible; mpv will render to whatever it is
     * given, and the target colorspace is set through the usual options.
     */
    int format;
    /**
     * VkImageUsageFlags the image was created with.
     */
    uint32_t usage;
    /**
     * Size in pixels.
     */
    int w, h;
    /**
     * VkImageLayout the image is in when mpv receives it. Use
     * VK_IMAGE_LAYOUT_UNDEFINED for an image whose contents do not matter,
     * which is the usual case for a frame that is about to be overwritten.
     */
    int layout;
} mpv_vulkan_image;

#ifdef __cplusplus
}
#endif

#endif

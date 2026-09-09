/*
 * WiVRn VR streaming
 * Copyright (C) 2025  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "video_encoder_pyrowave.h"

#include "encoder/encoder_settings.h"
#include "util/u_logging.h"
#include "utils/wivrn_vk_bundle.h"

namespace
{
class dummy_idr_handler : public wivrn::idr_handler
{
public:
	void on_feedback(const wivrn::from_headset::feedback &) override {};
	void reset() override {};
	bool should_skip(uint64_t frame_id) override
	{
		return false;
	};
};
vk::raii::CommandPool make_cmd_pool(wivrn::vk_bundle & vk, uint8_t stream_idx)
{
	auto res = vk.device.createCommandPool(vk::CommandPoolCreateInfo{

	        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer | vk::CommandPoolCreateFlagBits::eTransient,
	        .queueFamilyIndex = vk.queue.family_index,
	});
	vk.name(res, std::format("pyrowave encoder {} command pool", stream_idx));
	return res;
}
} // namespace

namespace wivrn
{
video_encoder_pyrowave::video_encoder_pyrowave(
        wivrn::vk_bundle & vk,
        const encoder_settings & settings,
        uint8_t stream_idx) :
        video_encoder(vk,
                      stream_idx,
                      vk.queue.family_index,
                      settings,
                      std::make_unique<dummy_idr_handler>(),
                      false),
        vk{vk},
        enc(vk.physical_device, vk.device, settings.width, settings.height, PyroWave::ChromaSubsampling::Chroma420),
        cmd_pool{make_cmd_pool(vk, stream_idx)},
        meta_size(enc.get_meta_required_size()),
        fps(settings.fps),
        encoded_size(settings.bitrate / (settings.fps * 8))
{
	if (settings.bit_depth != 8)
		throw std::runtime_error("pyrowave encoder only supports 8-bit encoding");

	auto command_buffers = vk.device.allocateCommandBuffers(
	        {.commandPool = *cmd_pool,
	         .commandBufferCount = num_slots});

	for (size_t i = 0; i < num_slots; ++i)
	{
		in[i].cmd = std::move(command_buffers[i]);
		vk.name(in[i].cmd, std::format("pyrowave {} command buffer {}", stream_idx, i));
		in[i].fence = vk::raii::Fence(vk.device, vk::FenceCreateInfo{.flags = vk::FenceCreateFlagBits::eSignaled});

		in[i].meta_buf = buffer_allocation(
		        vk.device,
		        {
		                .size = meta_size,
		                .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "pyrowave encoder meta buffer");
		in[i].data_buf = buffer_allocation(
		        vk.device,
		        {
		                .size = encoded_size + 2 * meta_size,
		                .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc,
		        },
		        {
		                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT,
		                .usage = VMA_MEMORY_USAGE_AUTO,
		        },
		        "pyrowave encoder data buffer");

		if (not(in[i].meta_buf.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		{
			in[i].meta_buf_staging = buffer_allocation(
			        vk.device,
			        {
			                .size = in[i].meta_buf.info().size,
			                .usage = vk::BufferUsageFlagBits::eTransferDst,
			        },
			        {
			                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
			                .usage = VMA_MEMORY_USAGE_AUTO,
			        },
			        "pyrowave encoder meta staging buffer");
		}
		if (not(in[i].data_buf.properties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
		{
			in[i].data_buf_staging = buffer_allocation(
			        vk.device,
			        {
			                .size = in[i].data_buf.info().size,
			                .usage = vk::BufferUsageFlagBits::eTransferDst,
			        },
			        {
			                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
			                .usage = VMA_MEMORY_USAGE_AUTO,
			        },
			        "pyrowave encoder data staging buffer");
		}
	}
}

void video_encoder_pyrowave::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo compositor_sem, uint8_t slot, uint64_t)
{
	if (vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_E("Timeout on stream %d", stream_idx);
		return;
	}

	if (auto framerate = pending_framerate.exchange(0))
		fps = framerate;
	if (auto bitrate = pending_bitrate.exchange(0))
		encoded_size = std::min<size_t>(bitrate / (fps * 8), in[slot].data_buf.info().size - 2 * meta_size);

	auto it = image_views.find(VkImage(y_cbcr));
	if (it == image_views.end())
	{
		auto y = vk.device.createImageView(
		        vk::ImageViewCreateInfo{
		                .image = y_cbcr,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eR8Unorm,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                        .levelCount = 1,
		                        .baseArrayLayer = stream_idx,
		                        .layerCount = 1,
		                }});
		auto cb = vk.device.createImageView(
		        vk::ImageViewCreateInfo{
		                .image = y_cbcr,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eR8G8Unorm,
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                        .levelCount = 1,
		                        .baseArrayLayer = stream_idx,
		                        .layerCount = 1,
		                }});
		auto cr = vk.device.createImageView(
		        vk::ImageViewCreateInfo{
		                .image = y_cbcr,
		                .viewType = vk::ImageViewType::e2D,
		                .format = vk::Format::eR8G8Unorm,
		                .components = {
		                        .r = vk::ComponentSwizzle::eG,
		                },
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                        .levelCount = 1,
		                        .baseArrayLayer = stream_idx,
		                        .layerCount = 1,
		                }});

		it = image_views.emplace(VkImage(y_cbcr), std::array{std::move(y), std::move(cb), std::move(cr)}).first;
	}
	std::array<vk::ImageView, 3> image_view{*it->second[0], *it->second[1], *it->second[2]};

	auto & cmd = in[slot].cmd;
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	PyroWave::Encoder::BitstreamBuffers buffers{
	        .meta = {
	                .buffer = in[slot].meta_buf,
	                .size = in[slot].meta_buf.info().size,
	        },
	        .bitstream = {
	                .buffer = in[slot].data_buf,
	                .size = in[slot].data_buf.info().size,
	        },
	        .target_size = encoded_size,
	};
	if (not enc.encode(cmd, image_view, buffers))
		U_LOG_W("pyrowave encode failed");

	if (in[slot].meta_buf_staging)
		cmd.copyBuffer(in[slot].meta_buf, in[slot].meta_buf_staging, vk::BufferCopy{.size = buffers.meta.size});
	if (in[slot].data_buf_staging)
		cmd.copyBuffer(in[slot].data_buf, in[slot].data_buf_staging, vk::BufferCopy{.size = buffers.bitstream.size});

	cmd.end();

	std::unique_lock lock(vk.queue.mutex);
	vk::CommandBufferSubmitInfo cmd_info{
	        .commandBuffer = *cmd,
	};
	compositor_sem.stageMask = vk::PipelineStageFlagBits2::eComputeShader;

	vk.device.resetFences(*in[slot].fence);
	vk.queue.queue.submit2(vk::SubmitInfo2{
	                               .waitSemaphoreInfoCount = 1,
	                               .pWaitSemaphoreInfos = &compositor_sem,
	                               .commandBufferInfoCount = 1,
	                               .pCommandBufferInfos = &cmd_info,
	                       },
	                       *in[slot].fence);
}

std::optional<video_encoder::data> video_encoder_pyrowave::encode(uint8_t slot, uint64_t frame_index)
{
	if (vk.device.waitForFences(*in[slot].fence, true, 1'000'000'000) == vk::Result::eTimeout)
	{
		U_LOG_W("Timeout on stream %d", stream_idx);
		return {};
	}

	auto & i = in[slot];
	const void * meta = i.meta_buf_staging ? i.meta_buf_staging.map() : i.meta_buf.map();
	const void * bitstream = i.data_buf_staging ? i.data_buf_staging.map() : i.data_buf.map();

	reordered_packet_buffer.resize(8 * 1024 * 1024);
	packets.resize(enc.compute_num_packets(meta, 8 * 1024));
	enc.packetize(
	        packets.data(),
	        8 * 1024,
	        reordered_packet_buffer.data(),
	        reordered_packet_buffer.size(),
	        meta,
	        bitstream);
	for (auto & p: packets)
		SendData({reordered_packet_buffer.data() + p.offset, p.size}, &p == &packets.back());

	return {};
}
} // namespace wivrn

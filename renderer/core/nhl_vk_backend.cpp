#include "renderer/core/nhl_vk_backend.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

// Win32 keyboard poll for the F9 hotkey capture (declared directly to keep
// <windows.h> out of this TU, mirroring the D3D12 path). No off-Windows
// equivalent is wired up yet, so the hotkey is simply never down there.
#ifdef _WIN32
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int v_key);
#endif

namespace nhl::graphics {

namespace {
std::atomic<uint32_t> g_seq{0};
std::mutex g_perf_mutex;
NhlVkPerfSnapshot g_perf;
}  // namespace

void PublishVkPerf(const NhlVkPerfSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(g_perf_mutex);
  g_perf = snapshot;
}

std::atomic<uint64_t> g_frame_index{0};

void PublishVkFrameIndex(uint64_t frame_index) {
  g_frame_index.store(frame_index, std::memory_order_relaxed);
}

uint64_t ReadVkFrameIndex() { return g_frame_index.load(std::memory_order_relaxed); }

NhlVkPerfSnapshot ReadVkPerf() {
  std::lock_guard<std::mutex> lock(g_perf_mutex);
  return g_perf;
}

std::string NhlVkGraphicsSystem::name() const {
  return "nhl-vulkan (rexglue ROV + fps tap)";
}

std::unique_ptr<rex::graphics::CommandProcessor>
NhlVkGraphicsSystem::CreateCommandProcessor() {
  // Mirror the D3D12 subclass: `this` is the VulkanGraphicsSystem the base CP ctor
  // expects; kernel_state() is the public GraphicsSystem accessor populated before
  // the command processor is created.
  return std::make_unique<NhlVkCommandProcessor>(this, kernel_state());
}

bool NhlVkCommandProcessor::IssueDraw(
    rex::graphics::xenos::PrimitiveType primitive_type, uint32_t index_count,
    rex::graphics::CommandProcessor::IndexBufferInfo* index_buffer_info,
    bool major_mode_explicit) {
  ++draws_this_frame_;
  namespace reg = rex::graphics::reg;
  namespace xe = rex::graphics::xenos;
  if (!register_file_) {
    return rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
        primitive_type, index_count, index_buffer_info, major_mode_explicit);
  }
  // Resolve experiment env once.
  if (!exp_resolved_) {
    exp_skip_dxt3_ = std::getenv("NHL_VK_SKIP_DXT3") != nullptr;
    exp_skip_alphatest_ = std::getenv("NHL_VK_SKIP_ALPHATEST") != nullptr;
    exp_skip_blend_ = std::getenv("NHL_VK_SKIP_BLEND") != nullptr;
    exp_skip_netlike_ = std::getenv("NHL_VK_NET_SKIP") != nullptr;
    exp_noblend_ = std::getenv("NHL_VK_NET_NOBLEND") != nullptr;
    if (const char* a = std::getenv("NHL_VK_SKIP_ADDR"))
      exp_skip_addr_ = uint32_t(std::strtoul(a, nullptr, 16));
    if (const char* a = std::getenv("NHL_VK_NET_ADDR"))
      exp_net_addr_ = uint32_t(std::strtoul(a, nullptr, 16));
    if (const char* r = std::getenv("NHL_VK_NET_REF")) { exp_ref_on_ = true; exp_ref_ = float(std::atof(r)); }
    if (const char* r = std::getenv("NHL_VK_NET_FORCE_AT")) { exp_force_at_on_ = true; exp_force_at_ref_ = float(std::atof(r)); }
    atoc_fix_on_ = std::getenv("NHL_VK_NO_ATOC_FIX") == nullptr;
    if (const char* r = std::getenv("NHL_VK_ATOC_REF")) atoc_ref_ = float(std::atof(r));
    // The diagnostic/inventory machinery (texture loop, skips, net-target
    // overrides) is only needed when something is asking for it; keep the
    // shipping path lean. The a2c fix below does NOT need it.
    diag_active_ = std::getenv("NHL_VK_DRAWLOG") || exp_skip_dxt3_ || exp_skip_alphatest_ ||
                   exp_skip_blend_ || exp_skip_netlike_ || exp_noblend_ || exp_ref_on_ ||
                   exp_force_at_on_ || exp_skip_addr_ != 0;
    exp_resolved_ = true;
  }

  // NHL_VK_RTLOG: log the guest's render-surface geometry once per distinct
  // configuration. The macOS port shows a hard corruption boundary at half the
  // framebuffer width in the 3D scene, so we need to know the surface pitch,
  // MSAA mode and scissor the guest actually programs.
  if (rtlog_on_) {
    const auto si = register_file_->Get<reg::RB_SURFACE_INFO>();
    const auto tl = register_file_->Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    const auto br = register_file_->Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    const auto wo = register_file_->Get<reg::PA_SC_WINDOW_OFFSET>();
    const auto ci = register_file_->Get<reg::RB_COLOR_INFO>();
    const uint32_t base_tiles =
        uint32_t(ci.color_base) | (uint32_t(ci.color_base_bit_11) << 11);
    const auto sc_key = register_file_->Get<reg::PA_SU_SC_MODE_CNTL>();
    // NHL_VK_SEQLOG: ground truth on ordering - emit a sequence-numbered event
    // whenever the tile pass changes, so it can be interleaved with the resolve
    // events below and the real draw/resolve order can be read off directly.
    if (seqlog_on_ && uint32_t(si.surface_pitch) == 640 &&
        uint32_t(si.msaa_samples) == 1u) {
      const auto ci_seq = register_file_->Get<reg::RB_COLOR_INFO>();
      const auto di_seq = register_file_->Get<reg::RB_DEPTH_INFO>();
      const uint32_t cbase = uint32_t(ci_seq.color_base) |
                             (uint32_t(ci_seq.color_base_bit_11) << 11);
      const uint32_t tl_x = uint32_t(tl.tl_x);
      // Include the window offset: that is what actually distinguishes the two
      // predicated-tiling passes, so keying without it merges them.
      const uint32_t seq_key = tl_x ^ (cbase << 12) ^ (uint32_t(br.br_y) << 20) ^
                               (uint32_t(int32_t(wo.window_x_offset) & 0xFFF) << 4);
      if (seq_key != last_seq_tile_) {
        last_seq_tile_ = seq_key;
        REXLOG_INFO(
            "[nhl-seq] {} DRAWS scissor=({},{})-({},{}) win_off=({},{}) cbase={} dbase={}",
            ++g_seq, uint32_t(tl.tl_x), uint32_t(tl.tl_y), uint32_t(br.br_x),
            uint32_t(br.br_y), int32_t(wo.window_x_offset), int32_t(wo.window_y_offset),
            cbase, uint32_t(di_seq.depth_base));
      }
    }
    const uint32_t key = (uint32_t(sc_key.vtx_window_offset_enable) << 30) ^
                         (uint32_t(tl.window_offset_disable) << 29) ^
                         (uint32_t(si.surface_pitch) << 8) ^
                         (uint32_t(si.msaa_samples) << 4) ^
                         (uint32_t(br.br_x) << 20) ^ uint32_t(br.br_y) ^
                         (base_tiles << 3) ^ (uint32_t(wo.window_x_offset) << 12);
    if (key != last_rt_key_) {
      last_rt_key_ = key;
      const auto sc = register_file_->Get<reg::PA_SU_SC_MODE_CNTL>();
      REXLOG_INFO(
          "[nhl-vk-rt] pitch={} msaa={} scissor=({},{})-({},{}) win_off=({},{}) "
          "color_base_tiles={} depth_base_tiles={} depth_en={} depth_write={} "
          "vtx_win_off_en={} scissor_win_off_dis={} msaa_en={}",
          uint32_t(si.surface_pitch), uint32_t(si.msaa_samples), uint32_t(tl.tl_x),
          uint32_t(tl.tl_y), uint32_t(br.br_x), uint32_t(br.br_y),
          int32_t(wo.window_x_offset), int32_t(wo.window_y_offset), base_tiles,
          uint32_t(register_file_->Get<reg::RB_DEPTH_INFO>().depth_base),
          uint32_t(register_file_->Get<reg::RB_DEPTHCONTROL>().z_enable),
          uint32_t(register_file_->Get<reg::RB_DEPTHCONTROL>().z_write_enable),
          uint32_t(sc.vtx_window_offset_enable),
          uint32_t(tl.window_offset_disable), uint32_t(sc.msaa_enable));
    }
  }

  // NHL_VK_SKIP_TILE=A|B: the 3D scene renders as two 640-pitch predicated-
  // tiling passes (tl_x 0 and 640) that are identical to the host. Rendering
  // only one of them separates "this pass is intrinsically broken" from "the
  // two passes interfere via the shared EDRAM".
  if (skip_tile_ != 0) {
    const auto si_t = register_file_->Get<reg::RB_SURFACE_INFO>();
    if (uint32_t(si_t.surface_pitch) == 640 &&
        uint32_t(si_t.msaa_samples) == 1u  /* MsaaSamples::k2X */) {
      const auto tl_t = register_file_->Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      const uint32_t tl_x = uint32_t(tl_t.tl_x);
      if ((skip_tile_ == 'A' && tl_x == 0) || (skip_tile_ == 'B' && tl_x == 640)) {
        ++skipped_draws_;
        return true;
      }
    }
  }

  {
    const auto si_d = register_file_->Get<reg::RB_SURFACE_INFO>();
    last_draw_pitch_ = uint32_t(si_d.surface_pitch);
  }

  const auto cc = register_file_->Get<reg::RB_COLORCONTROL>();
  if (cc.alpha_test_enable) ++alpha_test_draws_;
  if (cc.alpha_to_mask_enable) ++alpha_to_mask_draws_;
  const auto bc = register_file_->Get<reg::RB_BLENDCONTROL>();
  const bool blend_on =
      uint32_t(bc.color_srcblend) != 1u || uint32_t(bc.color_destblend) != 0u;
  if (blend_on) ++blend_draws_;

  // Diagnostics: per-window texture inventory + category skips, to identify which
  // draw is which (used to find that the net is an alpha-to-mask DXT3). Gated off
  // in the shipping path. The net = a DXT3 drawn with alpha-to-coverage, or
  // whatever binds the net addr (NHL_VK_NET_ADDR).
  bool net_target = false;
  if (diag_active_) {
    bool binds_dxt3 = false, binds_skip_addr = false, binds_net_addr = false;
    for (uint32_t i = 0; i < xe::kTextureFetchConstantCount; ++i) {
      const auto tf = register_file_->GetTextureFetch(i);
      if (tf.type != xe::FetchConstantType::kTexture) continue;
      const uint32_t base = tf.base_address << 12;
      if (tf.format == xe::TextureFormat::k_DXT2_3) binds_dxt3 = true;
      if (exp_skip_addr_ && base == exp_skip_addr_) binds_skip_addr = true;
      if (base == exp_net_addr_) binds_net_addr = true;
      // Dedup into the per-window inventory by base address; keep largest area seen.
      const uint32_t w = uint32_t(tf.size_2d.width) + 1u;
      const uint32_t h = uint32_t(tf.size_2d.height) + 1u;
      const uint32_t area = w * h;
      uint32_t slot = kInvMax;
      for (uint32_t j = 0; j < inv_count_; ++j) {
        if (inv_[j].base == base) { slot = j; break; }
      }
      if (slot == kInvMax && inv_count_ < kInvMax) slot = inv_count_++;
      if (slot != kInvMax) {
        TexDrawState& s = inv_[slot];
        s.draws++;
        if (!s.seen || area >= s.area) {  // record the dominant (largest) use
          s.seen = true; s.base = base; s.w = w; s.h = h; s.area = area;
          s.fmt = uint32_t(tf.format);
          s.alpha_test = cc.alpha_test_enable; s.alpha_to_mask = cc.alpha_to_mask_enable;
          s.blend = blend_on;
          s.blendctl0 = (*register_file_)[reg::RB_BLENDCONTROL::register_index];
          s.colorctl = (*register_file_)[reg::RB_COLORCONTROL::register_index];
          s.alpha_func = uint32_t(cc.alpha_func);
          s.alpha_ref = std::bit_cast<float>((*register_file_)[0x210E]);
          s.colormask = (*register_file_)[0x2104] & 0xFu;
        }
      }
    }
    net_target = (binds_dxt3 && cc.alpha_to_mask_enable) || binds_net_addr;
    const bool skip = (exp_skip_dxt3_ && binds_dxt3) ||
                      (exp_skip_alphatest_ && cc.alpha_test_enable) ||
                      (exp_skip_blend_ && blend_on) ||
                      (binds_skip_addr) ||
                      (exp_skip_netlike_ && net_target);
    if (skip) { ++skipped_draws_; return true; }
  }

  // THE FIX: emulate alpha-to-coverage with alpha-test for any draw whose only
  // alpha path is alpha-to-mask (FSI doesn't cull on a2c -> the rink net renders
  // opaque). Diagnostic experiment overrides on the net target compose on top.
  const bool atoc_fix = atoc_fix_on_ && cc.alpha_to_mask_enable && !cc.alpha_test_enable;
  const bool exp_override = net_target && (exp_ref_on_ || exp_noblend_ || exp_force_at_on_);
  if (atoc_fix || exp_override) {
    const uint32_t kAlphaRef = 0x210E;
    const uint32_t blend_idx = reg::RB_BLENDCONTROL::register_index;
    const uint32_t color_idx = reg::RB_COLORCONTROL::register_index;
    const uint32_t saved_ref = (*register_file_)[kAlphaRef];
    const uint32_t saved_blend = (*register_file_)[blend_idx];
    const uint32_t saved_color = (*register_file_)[color_idx];
    if (atoc_fix) {
      // alpha_func=Greater(4), alpha_test_enable=1, alpha_to_mask=0 in low 5 bits.
      (*register_file_)[color_idx] = (saved_color & ~0x1Fu) | 0x0Cu;
      (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(atoc_ref_);
    }
    // Diagnostic experiment overrides (take precedence on the net target).
    if (exp_ref_on_) (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(exp_ref_);
    if (exp_noblend_) (*register_file_)[blend_idx] = 0x00010001u;
    if (exp_force_at_on_) {
      (*register_file_)[color_idx] = (saved_color & ~0x1Fu) | 0x0Cu;
      (*register_file_)[kAlphaRef] = std::bit_cast<uint32_t>(exp_force_at_ref_);
    }
    const bool rv = rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
        primitive_type, index_count, index_buffer_info, major_mode_explicit);
    (*register_file_)[kAlphaRef] = saved_ref;
    (*register_file_)[blend_idx] = saved_blend;
    (*register_file_)[color_idx] = saved_color;
    return rv;
  }
  // Forward unchanged — rendering is identical to the stock Vulkan backend.
  return rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(
      primitive_type, index_count, index_buffer_info, major_mode_explicit);
}

// Resolve tap: NHL's 3D scene renders in two 640x720 EDRAM tiles that are
// identical from the host's point of view (same base/pitch after the window
// offset), so any left/right asymmetry in the final image has to come from the
// resolve destination, not the rendering. Log the copy rect + dest to see it.
bool NhlVkCommandProcessor::IssueCopy() {
  if (rtlog_on_ && register_file_) {
    const auto cc = register_file_->Get<rex::graphics::reg::RB_COPY_CONTROL>();
    const uint32_t dest_base =
        (*register_file_)[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
    const auto dest_pitch = register_file_->Get<rex::graphics::reg::RB_COPY_DEST_PITCH>();
    const auto di = register_file_->Get<rex::graphics::reg::RB_COPY_DEST_INFO>();
    const auto tl = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_SCISSOR_TL>();
    const auto br = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_SCISSOR_BR>();
    const auto wo = register_file_->Get<rex::graphics::reg::PA_SC_WINDOW_OFFSET>();
    const auto si_pre = register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>();
    // Log every resolve off the 640-pitch scene surface rather than a hardcoded
    // set of destination addresses, which go stale as soon as the game moves
    // its buffers.
    if (seqlog_on_) {
      const auto si_c = si_pre;
      REXLOG_INFO(
          "[nhl-seq] {} RESOLVE dest=0x{:08X} src_sel={} sample_sel={} msaa={} "
          "scissor=({},{})-({},{}) win_off={} clr_c={} clr_d={} depth_base={} src_pitch={} "
          "dest_pitch={} dest_height={} dest_fmt={} depth_fmt={} color_fmt={} rb_depth_clear=0x{:08X} exp_bias={}",
          ++g_seq, dest_base, uint32_t(cc.copy_src_select),
          uint32_t(cc.copy_sample_select), uint32_t(si_c.msaa_samples), uint32_t(tl.tl_x),
          uint32_t(tl.tl_y), uint32_t(br.br_x), uint32_t(br.br_y),
          int32_t(wo.window_x_offset), uint32_t(cc.color_clear_enable),
          uint32_t(cc.depth_clear_enable),
          uint32_t(register_file_->Get<rex::graphics::reg::RB_DEPTH_INFO>().depth_base),
          uint32_t(si_c.surface_pitch), uint32_t(dest_pitch.copy_dest_pitch),
          uint32_t(dest_pitch.copy_dest_height), uint32_t(di.copy_dest_format),
          uint32_t(register_file_->Get<rex::graphics::reg::RB_DEPTH_INFO>().depth_format),
          uint32_t(register_file_->Get<rex::graphics::reg::RB_COLOR_INFO>().color_format),
          uint32_t((*register_file_)[rex::graphics::XE_GPU_REG_RB_DEPTH_CLEAR]),
          int32_t(di.copy_dest_exp_bias));
    }
    const uint32_t key = dest_base ^ (uint32_t(tl.tl_x) << 3) ^
                         (uint32_t(br.br_x) << 15) ^
                         (uint32_t(dest_pitch.copy_dest_pitch) << 1);
    if (key != last_copy_key_) {
      last_copy_key_ = key;
      REXLOG_INFO(
          "[nhl-vk-copy] src_sel={} color_clear={} depth_clear={} cmd={} "
          "dest_base=0x{:08X} dest_pitch={} dest_height={} "
          "endian={} scissor=({},{})-({},{}) win_off=({},{})",
          uint32_t(cc.copy_src_select), uint32_t(cc.color_clear_enable),
          uint32_t(cc.depth_clear_enable), uint32_t(cc.copy_command), dest_base,
          uint32_t(dest_pitch.copy_dest_pitch), uint32_t(dest_pitch.copy_dest_height),
          uint32_t(di.copy_dest_endian), uint32_t(tl.tl_x), uint32_t(tl.tl_y),
          uint32_t(br.br_x), uint32_t(br.br_y), int32_t(wo.window_x_offset),
          int32_t(wo.window_y_offset));
    }
  }
  // NHL_VK_COPY_BARRIER: the 3D scene renders as two 640x720 EDRAM tiles that
  // share EDRAM base 0, so tile 1's draws overwrite exactly what tile 0's
  // resolve must read. That needs a real read->write (WAR) barrier between
  // them, and a write->read (RAW) barrier before each resolve. On MoltenVK one
  // of the two tiles always comes out corrupt, and *which* one flips with
  // render-pass handling, so force the render pass closed and flush pending
  // barriers on both sides of the resolve.
  if (copy_barrier_on_) {
    EndRenderPass();
    SubmitBarriers(/*force_end_render_pass=*/true);
    const bool ok = VulkanCommandProcessor::IssueCopy();
    EndRenderPass();
    SubmitBarriers(/*force_end_render_pass=*/true);
    return ok;
  }
  // NHL_VK_SKIP_UNFOLD: NHL's 3D scene renders in two 640-pitch EDRAM passes
  // whose per-pass resolves already land in the two halves of the SAME
  // destination texture (0x1AF1D000 == 0x1AF09000 + 0x14000 == +640px), so
  // together they compose the finished frame. The guest then issues a THIRD
  // resolve that re-reads the same EDRAM at pitch 1280 (the Xenos "un-fold")
  // and overwrites the whole texture. On MoltenVK that un-fold produces the
  // striped right half. Skipping it keeps the already-correct composition.
  // Only fires when the preceding draws used the narrow folded surface, so the
  // identical-looking full-screen resolve in menus is untouched.
  if (skip_unfold_ && register_file_) {
    const auto si_u = register_file_->Get<rex::graphics::reg::RB_SURFACE_INFO>();
    if (uint32_t(si_u.surface_pitch) == 1280u && last_draw_pitch_ == 640u) {
      last_draw_pitch_ = 0;  // one-shot per fold sequence
      return true;
    }
  }
  return VulkanCommandProcessor::IssueCopy();
}

void NhlVkCommandProcessor::PollHotkeyCapture() {
  if (!hotkey_checked_) {
    hotkey_checked_ = true;
    hotkey_enabled_ = std::getenv("NHL_HOTKEY_CAPTURE") != nullptr;
    if (hotkey_enabled_) {
      // Continue numbering after any existing gpu_trace/scene_NN so we never
      // overwrite the canonical captures (scene_00..scene_03).
      std::error_code ec;
      const std::filesystem::path root = "gpu_trace";
      if (std::filesystem::exists(root, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
          if (!e.is_directory()) continue;
          const std::string n = e.path().filename().string();
          if (n.rfind("scene_", 0) != 0) continue;
          char* end = nullptr;
          const unsigned long idx = std::strtoul(n.c_str() + 6, &end, 10);
          if (end && *end == '\0' && idx + 1 > hotkey_capture_index_) {
            hotkey_capture_index_ = uint32_t(idx + 1);
          }
        }
      }
      REXLOG_INFO(
          "[nhl-cap] F9 hotkey streaming capture enabled (press F9 in a GAMEPLAY "
          "scene to start, F9 again to stop -> gpu_trace/scene_{:02}/); replay with "
          "NHL_REPLAY_XTR / NHL_REPLAY_BENCH.",
          hotkey_capture_index_);
    }
  }
  if (!hotkey_enabled_) return;

  constexpr int kVkF9 = 0x78;
#ifdef _WIN32
  const bool down = (GetAsyncKeyState(kVkF9) & 0x8000) != 0;
#else
  const bool down = false;
#endif
  const bool rising = down && !hotkey_prev_down_;
  hotkey_prev_down_ = down;
  if (!rising) return;

  if (!hotkey_capturing_) {
    char dir[32];
    std::snprintf(dir, sizeof(dir), "scene_%02u", hotkey_capture_index_);
    const std::filesystem::path path = std::filesystem::path("gpu_trace") / dir;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
#ifdef NHL_HAVE_SDK_TRACING
    BeginTracing(path);  // streaming starts on the next primary-buffer execute
#endif
    hotkey_capturing_ = true;
    REXLOG_INFO("[nhl-cap] F9 capture BEGIN -> {}/ (frame {})", path.string(),
                frames_total_);
  } else {
#ifdef NHL_HAVE_SDK_TRACING
    EndTracing();
#endif
    hotkey_capturing_ = false;
    REXLOG_INFO("[nhl-cap] F9 capture END (frame {}) -> gpu_trace/scene_{:02}/",
                frames_total_, hotkey_capture_index_);
    ++hotkey_capture_index_;
  }
}

void NhlVkCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                      uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // Present the frame via the SDK's own Vulkan presenter, then sample timing.
  rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(
      frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  // F9 streaming capture toggle (gameplay-trace benchmark source).
  PollHotkeyCapture();

  const auto now = std::chrono::steady_clock::now();
  last_frame_draws_ = draws_this_frame_;
  last_alpha_test_ = alpha_test_draws_;
  last_alpha_to_mask_ = alpha_to_mask_draws_;
  last_blend_ = blend_draws_;
  last_skipped_ = skipped_draws_;
  alpha_test_draws_ = 0;
  alpha_to_mask_draws_ = 0;
  blend_draws_ = 0;
  skipped_draws_ = 0;
  window_draws_ += draws_this_frame_;
  draws_this_frame_ = 0;
  ++window_frames_;
  ++frames_total_;
  PublishVkFrameIndex(frames_total_);

  if (!started_) {
    started_ = true;
    window_start_ = now;
    window_frames_ = 0;
    window_draws_ = 0;
    return;
  }

  const double elapsed = std::chrono::duration<double>(now - window_start_).count();
  if (elapsed >= 1.0 && window_frames_ > 0) {
    const double fps = window_frames_ / elapsed;
    const double frame_ms = 1000.0 * elapsed / double(window_frames_);
    const double avg_draws = double(window_draws_) / double(window_frames_);
    REXLOG_INFO(
        "[nhl-vk-fps] fps={:.1f} frame_ms={:.2f} draws/frame={:.0f} (last_frame_draws={}) "
        "frames_total={}",
        fps, frame_ms, avg_draws, last_frame_draws_, frames_total_);
    // Publish for the enhancements overlay's perf HUD.
    PublishVkPerf(NhlVkPerfSnapshot{fps, frame_ms, avg_draws, frames_total_, true});
    if (std::getenv("NHL_VK_DRAWLOG")) {
      REXLOG_INFO(
          "[nhl-vk-draw] last_frame: draws={} alpha_test={} alpha_to_mask={} blend={} skipped={}",
          last_frame_draws_, last_alpha_test_, last_alpha_to_mask_, last_blend_, last_skipped_);
      // Texture inventory, largest area first. The visible net should be findable
      // here by its dimensions; then NHL_VK_SKIP_ADDR=<base> confirms it.
      uint32_t order[kInvMax];
      for (uint32_t i = 0; i < inv_count_; ++i) order[i] = i;
      for (uint32_t a = 0; a < inv_count_; ++a)
        for (uint32_t b = a + 1; b < inv_count_; ++b)
          if (inv_[order[b]].area > inv_[order[a]].area) std::swap(order[a], order[b]);
      const uint32_t show = inv_count_ < 24u ? inv_count_ : 24u;
      for (uint32_t k = 0; k < show; ++k) {
        const TexDrawState& s = inv_[order[k]];
        REXLOG_INFO(
            "[nhl-vk-draw]   tex base=0x{:08X} dims={}x{} fmt={} draws={} | at={} a2m={} "
            "blend={} | BLENDCTL0=0x{:08X} COLORCTL=0x{:08X} func={} ref={:.3f} cmask=0x{:X}",
            s.base, s.w, s.h, s.fmt, s.draws, s.alpha_test, s.alpha_to_mask, s.blend,
            s.blendctl0, s.colorctl, s.alpha_func, s.alpha_ref, s.colormask);
      }
    }
    for (uint32_t i = 0; i < inv_count_; ++i) inv_[i] = TexDrawState{};
    inv_count_ = 0;
    window_start_ = now;
    window_frames_ = 0;
    window_draws_ = 0;
  }
}

}  // namespace nhl::graphics

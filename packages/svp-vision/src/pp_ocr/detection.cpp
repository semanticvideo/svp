#include "pp_ocr_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace svp::vision::pp_ocr_internal {

DetInputImage preprocess_detection(
    const ColorRasterFrame& frame,
    int limit_side_len) {
  const int src_w = frame.width;
  const int src_h = frame.height;
  double ratio = 1.0;
  if (std::max(src_h, src_w) > limit_side_len) {
    ratio = src_h > src_w
        ? static_cast<double>(limit_side_len) / src_h
        : static_cast<double>(limit_side_len) / src_w;
  }
  int resized_h = static_cast<int>(src_h * ratio);
  int resized_w = static_cast<int>(src_w * ratio);
  resized_h = ((resized_h + 31) / 32) * 32;
  resized_w = ((resized_w + 31) / 32) * 32;
  if (resized_h < 32) resized_h = 32;
  if (resized_w < 32) resized_w = 32;

  DetInputImage img;
  img.width = resized_w;
  img.height = resized_h;
  img.ratio = ratio;
  img.data.resize(static_cast<std::size_t>(3) * resized_h * resized_w);

  const float mean[] = {0.485f, 0.456f, 0.406f};
  const float std_val[] = {0.229f, 0.224f, 0.225f};

  for (int c = 0; c < 3; ++c) {
    const int src_c = (c == 0) ? 2 : (c == 1) ? 1 : 0;
    for (int y = 0; y < resized_h; ++y) {
      const double src_y = static_cast<double>(y) / ratio;
      const int y0 = static_cast<int>(std::min(src_y, static_cast<double>(src_h - 1)));
      const int y1c = std::min(y0 + 1, src_h - 1);
      const double fy = src_y - y0;
      for (int x = 0; x < resized_w; ++x) {
        const double src_x = static_cast<double>(x) / ratio;
        const int x0 = static_cast<int>(std::min(src_x, static_cast<double>(src_w - 1)));
        const int x1c = std::min(x0 + 1, src_w - 1);
        const double fx = src_x - x0;

        const auto& p00 = frame.pixels[y0 * src_w + x0];
        const auto& p01 = frame.pixels[y0 * src_w + x1c];
        const auto& p10 = frame.pixels[y1c * src_w + x0];
        const auto& p11 = frame.pixels[y1c * src_w + x1c];

        const uint8_t v00 = (src_c == 0) ? p00.r : (src_c == 1) ? p00.g : p00.b;
        const uint8_t v01 = (src_c == 0) ? p01.r : (src_c == 1) ? p01.g : p01.b;
        const uint8_t v10 = (src_c == 0) ? p10.r : (src_c == 1) ? p10.g : p10.b;
        const uint8_t v11 = (src_c == 0) ? p11.r : (src_c == 1) ? p11.g : p11.b;

        double val = (1.0 - fx) * (1.0 - fy) * v00 +
                     fx * (1.0 - fy) * v01 +
                     (1.0 - fx) * fy * v10 +
                     fx * fy * v11;
        val = val / 255.0;
        val = (val - mean[c]) / std_val[c];

        img.data[c * resized_h * resized_w + y * resized_w + x] =
            static_cast<float>(val);
      }
    }
  }
  return img;
}

std::vector<DetBox> db_postprocess(
    const float* pred_data,
    int pred_h,
    int pred_w,
    double ratio,
    double thresh,
    double box_thresh,
    double unclip_ratio) {
  std::vector<uint8_t> mask(pred_h * pred_w, 0);
  for (int i = 0; i < pred_h * pred_w; ++i) {
    mask[i] = (pred_data[i] > thresh) ? 1 : 0;
  }

  std::vector<int> labels(pred_h * pred_w, 0);
  int num_labels = 0;
  std::vector<DetBox> boxes;

  const int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
  const int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};

  for (int y = 0; y < pred_h; ++y) {
    for (int x = 0; x < pred_w; ++x) {
      const int idx = y * pred_w + x;
      if (mask[idx] == 0 || labels[idx] != 0) continue;

      ++num_labels;
      int min_x = x, min_y = y, max_x = x, max_y = y;
      double sum_score = 0.0;
      int count = 0;

      std::vector<std::pair<int, int>> stack;
      stack.push_back({x, y});
      labels[idx] = num_labels;

      while (!stack.empty()) {
        auto [cx, cy] = stack.back();
        stack.pop_back();

        min_x = std::min(min_x, cx);
        min_y = std::min(min_y, cy);
        max_x = std::max(max_x, cx);
        max_y = std::max(max_y, cy);

        sum_score += pred_data[cy * pred_w + cx];
        ++count;

        for (int d = 0; d < 8; ++d) {
          const int nx = cx + dx[d];
          const int ny = cy + dy[d];
          if (nx < 0 || nx >= pred_w || ny < 0 || ny >= pred_h) continue;
          const int nidx = ny * pred_w + nx;
          if (mask[nidx] == 0 || labels[nidx] != 0) continue;
          labels[nidx] = num_labels;
          stack.push_back({nx, ny});
        }
      }

      if (count > 0) {
        double avg_score = sum_score / count;
        if (avg_score < box_thresh) continue;
      }

      const int bw = max_x - min_x + 1;
      const int bh = max_y - min_y + 1;
      if (bw < 3 || bh < 3) continue;

      const int expand_w = static_cast<int>(bw * (unclip_ratio - 1.0) / 2.0);
      const int expand_h = static_cast<int>(bh * (unclip_ratio - 1.0) / 2.0);

      DetBox box;
      box.left = static_cast<int>((min_x - expand_w) / ratio);
      box.top = static_cast<int>((min_y - expand_h) / ratio);
      box.right = static_cast<int>((max_x + expand_w) / ratio);
      box.bottom = static_cast<int>((max_y + expand_h) / ratio);
      boxes.push_back(box);
    }
  }

  return boxes;
}

}  // namespace svp::vision::pp_ocr_internal

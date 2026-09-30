#pragma once

#include <memory>
#include <ostream>

namespace svp::progress {

// Output stream that writes through a private duplicate of a file descriptor.
//
// Progress output must keep reaching the destination chosen when the stream
// was opened, even while other code temporarily repoints the original
// descriptor (for example dup2(/dev/null, STDERR_FILENO) around noisy library
// initialisation on another thread). The duplicate refers to the open file
// description the original fd named at capture time, so later dup2() calls on
// the original fd cannot redirect or swallow writes made through this stream.
//
// The stream is unbuffered: each insertion is written immediately, like
// std::cerr, so a line inserted as one string reaches the fd in one write.
// If the descriptor cannot be duplicated (for example it is closed), fd()
// returns -1 and writes are discarded, matching std::cerr on a closed fd.
class FdStream {
 public:
  explicit FdStream(int fd);
  ~FdStream();

  FdStream(const FdStream&) = delete;
  FdStream& operator=(const FdStream&) = delete;

  int fd() const noexcept;
  std::ostream& stream() noexcept;

 private:
  class Buffer;
  std::unique_ptr<Buffer> buffer_;
  std::ostream stream_;
};

}  // namespace svp::progress

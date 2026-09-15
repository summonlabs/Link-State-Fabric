// Real operating system process proofs.
//
// This suite starts the real coordinator and worker executables as separate OS
// processes, kills them abruptly with TerminateProcess and proves that worker
// death, permanent incarnation fencing, conservative recovery and coordinator
// restart behave as the runtime claims. There are no timeouts anywhere: every
// wait is a blocking read of the child's own output, so a hang is a defect that
// shows up as a hung test rather than as a masked failure.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "linkstate/client.hpp"
#include "linkstate/engine.hpp"
#include "lsf_test.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#if !defined(LSF_COORDINATOR_EXE) || !defined(LSF_WORKER_EXE)
#define LSF_MULTIPROCESS_UNAVAILABLE 1
#endif

using namespace linkstate;

namespace {

#ifdef _WIN32

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string narrow_text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(),
                      size, nullptr, nullptr);
  return narrow_text;
}

/// A real child process with a captured stdout/stderr stream.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ~ChildProcess() {
    (void)terminate();
    release();
  }

  bool start(const std::string& executable, const std::vector<std::string>& arguments,
             const std::string& working_directory) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    attributes.lpSecurityDescriptor = nullptr;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &attributes, 0)) {
      return false;
    }
    if (!CreatePipe(&stdin_read, &stdin_write, &attributes, 0)) {
      CloseHandle(stdout_read);
      CloseHandle(stdout_write);
      return false;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    std::string command_line = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command_line += " \"" + argument + "\"";
    }
    std::wstring wide_command = widen(command_line);
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stdout_write;
    startup.hStdInput = stdin_read;

    PROCESS_INFORMATION information{};
    const std::wstring wide_directory = widen(working_directory);
    const BOOL created =
        CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr,
                       wide_directory.empty() ? nullptr : wide_directory.c_str(), &startup,
                       &information);
    CloseHandle(stdout_write);
    CloseHandle(stdin_read);
    if (!created) {
      CloseHandle(stdout_read);
      CloseHandle(stdin_write);
      return false;
    }
    process_ = information.hProcess;
    thread_ = information.hThread;
    stdout_read_ = stdout_read;
    stdin_write_ = stdin_write;
    return true;
  }

  /// Blocking read of one line of child output. Returns an empty string at end
  /// of stream. No timeout is applied: the caller waits for real output.
  std::string read_line() {
    for (;;) {
      const std::size_t newline = buffer_.find('\n');
      if (newline != std::string::npos) {
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      char chunk[512];
      DWORD received = 0;
      const BOOL ok = ReadFile(stdout_read_, chunk, sizeof(chunk), &received, nullptr);
      if (!ok || received == 0) {
        if (!buffer_.empty()) {
          std::string line = buffer_;
          buffer_.clear();
          return line;
        }
        return std::string();
      }
      buffer_.append(chunk, received);
    }
  }

  /// Blocks until a line containing the marker is observed. Returns the empty
  /// string when the child closed its output first.
  std::string wait_for(const std::string& marker) {
    for (;;) {
      const std::string line = read_line();
      if (line.empty()) {
        return std::string();
      }
      if (line.find(marker) != std::string::npos) {
        return line;
      }
    }
  }

  bool terminate() {
    if (process_ == nullptr) {
      return false;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE) {
      (void)TerminateProcess(process_, 3);
    }
    return true;
  }

  int wait() {
    if (process_ == nullptr) {
      return -1;
    }
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return static_cast<int>(code);
  }

  void close_stdin() {
    if (stdin_write_ != nullptr) {
      CloseHandle(stdin_write_);
      stdin_write_ = nullptr;
    }
  }

 private:
  void release() {
    close_stdin();
    if (stdout_read_ != nullptr) {
      CloseHandle(stdout_read_);
      stdout_read_ = nullptr;
    }
    if (thread_ != nullptr) {
      CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (process_ != nullptr) {
      CloseHandle(process_);
      process_ = nullptr;
    }
  }

  HANDLE process_ = nullptr;
  HANDLE thread_ = nullptr;
  HANDLE stdout_read_ = nullptr;
  HANDLE stdin_write_ = nullptr;
  std::string buffer_;
};

#endif  // _WIN32

struct Scratch {
  std::filesystem::path directory;
  std::filesystem::path journal;
  std::filesystem::path bind_file;

  Scratch() {
    directory = std::filesystem::temp_directory_path() /
                ("lsf-multiprocess-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    journal = directory / "coordinator.lsfjournal";
    bind_file = directory / "links.txt";
  }

  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }

  void write_bind_file(const std::vector<std::string>& lines) const {
    std::ofstream output(bind_file, std::ios::trunc);
    for (const std::string& line : lines) {
      output << line << "\n";
    }
  }
};

std::uint16_t parse_port(const std::string& endpoint_line) {
  const std::size_t separator = endpoint_line.rfind(':');
  LSF_REQUIRE(separator != std::string::npos);
  return static_cast<std::uint16_t>(std::stoi(endpoint_line.substr(separator + 1)));
}

std::string endpoint_of(const std::string& line) {
  const std::size_t space = line.find(' ');
  return space == std::string::npos ? std::string() : line.substr(space + 1);
}

PublisherClientConfig query_client(const std::string& address, std::uint16_t port,
                                   const std::string& tag,
                                   const std::vector<LinkId>& links) {
  PublisherClientConfig config;
  config.address = address;
  config.port = port;
  config.publisher = lsf_test::publisher_id("probe-" + tag);
  config.worker_boot = lsf_test::boot_id("probe-boot-" + tag);
  config.source = lsf_test::source_id("probe-source-" + tag);
  config.links = links;
  config.agent_name = "probe";
  return config;
}

PublishEvidenceRequest request_for(const PublisherClientConfig& config,
                                   const CoordinatorEpoch& epoch, const LinkId& link,
                                   EvidenceClaim claim, std::uint64_t sequence,
                                   const std::string& tag) {
  PublishEvidenceRequest publication;
  publication.publication = PublicationId::from_validated("pub." + tag);
  EvidenceRecord& observation = publication.observation;
  observation.key.link = link;
  observation.key.source = config.source;
  observation.key.kind = EvidenceKind::SyntheticTest;
  observation.observation = ObservationId::from_validated("obs." + tag);
  observation.publisher = config.publisher;
  observation.worker_boot = config.worker_boot;
  observation.epoch = epoch;
  observation.source_generation = SourceGeneration::from_value(1);
  observation.sequence = sequence;
  observation.topology_generation = TopologyGeneration::from_value(1);
  observation.evidence_class = EvidenceClass::Synthetic;
  observation.claim = claim;
  return publication;
}

}  // namespace

#if defined(LSF_MULTIPROCESS_UNAVAILABLE)

LSF_TEST(real_process_proofs_require_the_coordinator_and_worker_tools) {
  std::printf("  the multiprocess proof suite requires LSF_BUILD_TOOLS=ON\n");
  std::fflush(stdout);
}

#else

LSF_TEST(real_worker_death_incarnation_fencing_and_coordinator_restart) {
  Scratch scratch;
  scratch.write_bind_file({
      "# link local-endpoint remote-endpoint class topology-generation endpoint-generation",
      "link.proof.one ep.proof.one.a ep.proof.one.b ETHERNET_LIKE 1 1",
      "link.proof.two ep.proof.two.a ep.proof.two.b ETHERNET_LIKE 1 1",
  });

  ChildProcess coordinator;
  LSF_REQUIRE(coordinator.start(LSF_COORDINATOR_EXE,
                                {"--journal", scratch.journal.string(), "--bind-file",
                                 scratch.bind_file.string(), "--port", "0", "--name",
                                 "proof-coordinator"},
                                ""));
  // The coordinator banner is emitted in a fixed order: bound links, endpoint,
  // epoch, link count, ready.
  const std::string bound_line = coordinator.wait_for("bound ");
  LSF_REQUIRE(!bound_line.empty());
  LSF_CHECK(bound_line.find("bound 2 links") != std::string::npos);
  const std::string endpoint_line = coordinator.wait_for("endpoint ");
  LSF_REQUIRE(!endpoint_line.empty());
  const std::string endpoint = endpoint_of(endpoint_line);
  const std::size_t separator = endpoint.rfind(':');
  const std::string address = endpoint.substr(0, separator);
  const std::uint16_t port = parse_port(endpoint_line);
  LSF_REQUIRE(!coordinator.wait_for("ready").empty());
  std::printf("  coordinator endpoint %s\n", endpoint.c_str());
  std::fflush(stdout);

  const LinkId first = lsf_test::link_id("proof.one");
  const LinkId second = lsf_test::link_id("proof.two");

  // Publisher A is the sole source for link two and one of two sources for link
  // one; publisher B corroborates link one.
  ChildProcess worker_a;
  LSF_REQUIRE(worker_a.start(LSF_WORKER_EXE,
                             {"--endpoint", endpoint, "--publisher", "worker-a", "--boot",
                              "boot-a-1", "--source", "source-a", "--link", first.value(),
                              "--link", second.value(), "--claim", "UP", "--hold"},
                             ""));
  LSF_CHECK(worker_a.wait_for("publish " + first.value()).find("COMMITTED") != std::string::npos);
  LSF_CHECK(worker_a.wait_for("publish " + second.value()).find("COMMITTED") != std::string::npos);

  ChildProcess worker_b;
  LSF_REQUIRE(worker_b.start(LSF_WORKER_EXE,
                             {"--endpoint", endpoint, "--publisher", "worker-b", "--boot",
                              "boot-b-1", "--source", "source-b", "--link", first.value(),
                              "--claim", "UP", "--hold"},
                             ""));
  LSF_CHECK(worker_b.wait_for("publish " + first.value()).find("COMMITTED") != std::string::npos);

  PublisherClientConfig probe = query_client(address, port, "one", {first, second});
  PublisherClient probe_client(probe);
  LSF_REQUIRE(probe_client.connect().code == OutcomeCode::Committed);
  LinkStateView view;
  LSF_REQUIRE(probe_client.query_state(first, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);
  LSF_REQUIRE(probe_client.query_state(second, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);

  // Real worker death: terminate publisher A without any graceful shutdown.
  LSF_REQUIRE(worker_a.terminate());
  const int worker_a_exit = worker_a.wait();
  std::printf("  worker A terminated with exit code %d\n", worker_a_exit);
  std::fflush(stdout);

  // The coordinator detects the closed control connection and fences A.
  const std::string closed = coordinator.wait_for("session closed");
  LSF_REQUIRE(!closed.empty());
  LSF_CHECK(closed.find("fenced=1") != std::string::npos);
  const std::string fenced = coordinator.wait_for("fenced publisher=worker-a");
  LSF_REQUIRE(!fenced.empty());
  LSF_CHECK(fenced.find("boot=boot-a-1") != std::string::npos);
  std::printf("  coordinator reported: %s\n", closed.c_str());
  std::fflush(stdout);

  // A was the sole source of link two: it must require revalidation, never DOWN.
  LSF_REQUIRE(probe_client.query_state(second, view));
  LSF_CHECK(view.state() == LinkOperationalState::RevalidationRequired);
  // B's independent evidence for link one is preserved.
  LSF_REQUIRE(probe_client.query_state(first, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);

  // Replaying from the dead incarnation is rejected permanently.
  ChildProcess worker_a_replay;
  LSF_REQUIRE(worker_a_replay.start(LSF_WORKER_EXE,
                                    {"--endpoint", endpoint, "--publisher", "worker-a", "--boot",
                                     "boot-a-1", "--source", "source-a", "--link", second.value(),
                                     "--claim", "UP", "--hold"},
                                    ""));
  LSF_CHECK(!worker_a_replay.wait_for("register_failed").empty());
  worker_a_replay.terminate();
  (void)worker_a_replay.wait();

  // A fresh incarnation must present fresh evidence to become current again.
  ChildProcess worker_a_fresh;
  LSF_REQUIRE(worker_a_fresh.start(LSF_WORKER_EXE,
                                   {"--endpoint", endpoint, "--publisher", "worker-a", "--boot",
                                    "boot-a-2", "--source", "source-a", "--link", second.value(),
                                    "--claim", "UP", "--hold"},
                                   ""));
  const std::string fresh =
      worker_a_fresh.wait_for("publish " + second.value());
  LSF_CHECK(fresh.find("COMMITTED") != std::string::npos);
  LSF_CHECK(fresh.find("state=UP") != std::string::npos);
  LSF_REQUIRE(probe_client.query_state(second, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);
  probe_client.abandon();

  // Real coordinator death without a graceful shutdown. The journal holds the
  // durable state because the coordinator writes it after every accepted
  // mutation.
  LSF_REQUIRE(coordinator.terminate());
  (void)coordinator.wait();

  ChildProcess restarted;
  LSF_REQUIRE(restarted.start(LSF_COORDINATOR_EXE,
                              {"--journal", scratch.journal.string(), "--bind-file",
                               scratch.bind_file.string(), "--port", "0", "--name",
                               "proof-coordinator-2"},
                              ""));
  const std::string recovered = restarted.wait_for("recovered ");
  LSF_REQUIRE(!recovered.empty());
  const std::string epoch_line = restarted.wait_for("epoch advanced");
  LSF_REQUIRE(!epoch_line.empty());
  LSF_REQUIRE(!restarted.wait_for("bound ").empty());
  const std::string restarted_endpoint_line = restarted.wait_for("endpoint ");
  LSF_REQUIRE(!restarted_endpoint_line.empty());
  LSF_REQUIRE(!restarted.wait_for("ready").empty());
  const std::string restarted_endpoint = endpoint_of(restarted_endpoint_line);
  const std::size_t restarted_separator = restarted_endpoint.rfind(':');
  const std::string restarted_address = restarted_endpoint.substr(0, restarted_separator);
  const std::uint16_t restarted_port = parse_port(restarted_endpoint_line);
  std::printf("  recovered: %s; %s\n", recovered.c_str(), epoch_line.c_str());
  std::fflush(stdout);
  LSF_CHECK(recovered.find("durable records") != std::string::npos);
  LSF_CHECK(epoch_line.find("1") != std::string::npos);

  // Durable structure survived, live authority did not.
  PublisherClientConfig probe_two = query_client(restarted_address, restarted_port, "two",
                                                 {first, second});
  PublisherClient probe_two_client(probe_two);
  LSF_REQUIRE(probe_two_client.connect().code == OutcomeCode::Committed);
  LSF_REQUIRE(probe_two_client.epoch().value() == 1);
  LSF_REQUIRE(probe_two_client.query_state(second, view));
  LSF_CHECK(view.state() == LinkOperationalState::RevalidationRequired);
  LSF_REQUIRE(probe_two_client.query_state(first, view));
  LSF_CHECK(view.state() == LinkOperationalState::RevalidationRequired);

  // Traffic from the previous coordinator epoch is rejected.
  LSF_REQUIRE(probe_two_client.register_publisher().code == OutcomeCode::Committed);
  PublishEvidenceRequest stale_epoch =
      request_for(probe_two, CoordinatorEpoch::from_value(0), second, EvidenceClaim::Up, 1,
                  "stale-epoch");
  const Outcome stale = probe_two_client.publish(stale_epoch);
  LSF_CHECK(stale.code == OutcomeCode::StaleCoordinatorEpoch);
  const Outcome current = probe_two_client.publish(
      request_for(probe_two, probe_two_client.epoch(), second, EvidenceClaim::Up, 2, "fresh-epoch"));
  LSF_REQUIRE(current.code == OutcomeCode::Committed);
  LSF_REQUIRE(probe_two_client.query_state(second, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);

  // Fencing is permanent across a coordinator restart: the dead incarnation of
  // worker A is still refused by the restarted coordinator.
  ChildProcess worker_a_replay_two;
  LSF_REQUIRE(worker_a_replay_two.start(LSF_WORKER_EXE,
                                        {"--endpoint", restarted_endpoint, "--publisher",
                                         "worker-a", "--boot", "boot-a-1", "--source", "source-a",
                                         "--link", second.value(), "--claim", "UP", "--hold"},
                                        ""));
  const std::string replay_line =
      worker_a_replay_two.wait_for("register_failed");
  LSF_CHECK(replay_line.find("STALE_AUTHORITY") != std::string::npos);
  worker_a_replay_two.terminate();
  (void)worker_a_replay_two.wait();

  // A worker that was never fenced re-registers under the new epoch and must
  // publish fresh evidence before its link is authoritative again.
  ChildProcess worker_b_again;
  LSF_REQUIRE(worker_b_again.start(LSF_WORKER_EXE,
                                   {"--endpoint", restarted_endpoint, "--publisher", "worker-b",
                                    "--boot", "boot-b-1", "--source", "source-b", "--link",
                                    first.value(), "--claim", "UP", "--hold"},
                                   ""));
  const std::string reregistered = worker_b_again.wait_for("publish " + first.value());
  LSF_CHECK(reregistered.find("COMMITTED") != std::string::npos);
  LSF_CHECK(reregistered.find("state=UP") != std::string::npos);
  LSF_REQUIRE(probe_two_client.query_state(first, view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);
  probe_two_client.abandon();

  // Shut every remaining child down and make sure none of them survives.
  worker_b.terminate();
  (void)worker_b.wait();
  worker_a_fresh.terminate();
  (void)worker_a_fresh.wait();
  worker_b_again.terminate();
  (void)worker_b_again.wait();
  restarted.terminate();
  (void)restarted.wait();
  std::printf("  all child processes terminated\n");
  std::fflush(stdout);
}

#endif  // LSF_MULTIPROCESS_UNAVAILABLE

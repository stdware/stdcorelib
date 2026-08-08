// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_POPEN_H
#define STDCORELIB_POPEN_H

#include <filesystem>
#include <vector>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <system_error>
#include <functional>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup process Processes and libraries
///
/// Starting a child process and loading a shared object, on both Windows and POSIX.
///
/// stdc::Popen is a port of Python's \c subprocess.Popen. The pipes are \c std::iostream, so the
/// usual vocabulary works on them, and stdc::Popen::communicate() is the one to reach for when more
/// than one is open: draining them by hand, one at a time, deadlocks as soon as the other fills.
///
/// \code
///     using namespace stdc;
///
///     Popen proc;
///     proc.args({"git", "describe", "--tags"})
///         .stdin_(Popen::DEVNULL)
///         .stdout_(Popen::PIPE)
///         .stderr_(Popen::STDOUT);
///
///     std::string err;
///     if (!proc.start(&err)) {
///         return err;
///     }
///     auto [out, _] = proc.communicate({}, 5000);
///     int code = proc.returncode().value_or(-1);
/// \endcode
///
/// A Popen owns its child and kills it on the way out. \c detached(true) gives that up: the
/// child is launched independently and this process keeps nothing but its pid.
///
/// stdc::SharedLibrary loads one at run time and resolves symbols from it.
/// \c SharedLibrary::setLibraryPath() is what lets a plugin outside the usual directories find the
/// libraries next to it, and \c locateLibraryPath() answers where a given address came from.
///
/// \code
///     SharedLibrary lib;
///     if (!lib.open(plugin_path)) {
///         return lib.lastError();
///     }
///     auto entry = reinterpret_cast<int (*)()>(lib.resolve("plugin_init"));
/// \endcode

namespace stdc {

    /// \addtogroup process
    /// @{

    /// Creates and controls a child process, after Python's \c subprocess.Popen.
    ///
    /// The setters build the process up and return \c *this, so they chain. Nothing happens
    /// until start(). After that the child is running and the streams that were set to \c PIPE
    /// are open.
    ///
    /// \code
    ///   Popen proc;
    ///   proc.args({"git", "--version"})
    ///       .stdin_(Popen::DEVNULL)
    ///       .stdout_(Popen::PIPE)
    ///       .stderr_(Popen::STDOUT); // fold stderr into the stdout pipe
    ///
    ///   std::string err;
    ///   if (!proc.start(&err)) {
    ///       return err;
    ///   }
    ///   auto [out, _] = proc.communicate();
    ///   int code = proc.returncode().value_or(-1);
    /// \endcode
    ///
    /// Every \c std::string here is UTF-8, as everywhere else in this library: args(), env(),
    /// what communicate() is given and what it hands back. On Windows they are converted to
    /// UTF-16 on the way to \c CreateProcess, so an argument written in any script arrives as it
    /// was meant rather than in the system code page. What executable() and cwd() take is a
    /// \c std::filesystem::path, which carries its own encoding and is passed on as it is.
    ///
    /// \note The three stream names end in an underscore because \c stdin, \c stdout and
    ///       \c stderr are macros, which no namespace or class can hide. Everything else here
    ///       is spelled the way Python spells it.
    ///
    /// \warning Reading a pipe by hand rather than through communicate() works for one pipe,
    ///          not for two. A pipe blocks its writer once full, so a child filling stderr while
    ///          the parent is still draining stdout waits forever. communicate() exists to get
    ///          this right.
    ///
    /// \sa https://docs.python.org/3/library/subprocess.html
    class STDC_EXPORT Popen {
    public:
        /// What to connect a standard stream to, beyond a descriptor or a \c FILE * of your own.
        enum IOType {
            PIPE = 1, ///< a new pipe, readable or writable from this side afterwards
            DEVNULL,  ///< the null device
            STDOUT,   ///< stderr_() only: send it wherever stdout goes
        };

        struct IODev {
            enum Kind {
                None,
                Builtin,
                FD,
                CFile,
            };
            IODev() : kind(None) {
            }
            IODev(IOType builtin) : kind(Builtin) {
                data.builtin = builtin;
            }
            IODev(int fd) : kind(FD) {
                data.fd = fd;
            }
            IODev(FILE *file) : kind(CFile) {
                data.file = file;
            }
            int kind;
            union {
                IOType builtin;
                int fd;
                FILE *file;
            } data;
        };

#ifdef _WIN32
        struct StartupInfo {
            // winapi members
            uint32_t dwFlags;
            void *hStdInput;
            void *hStdOutput;
            void *hStdError;
            uint16_t wShowWindow;

            // supported keys:
            //     handle_list: INVALID_HANDLE_VALUE terminated list of HANDLE to be inherited
            std::map<std::string, void *> lpAttributeList;
        };

        enum WindowsSignal {
            WS_CTRL_C_EVENT = 0,
            WS_CTRL_BREAK_EVENT = 1,
        };
#endif

        /// One end of a pipe to the child, as an ordinary \c std::iostream.
        ///
        /// Only open for a stream that was set to \c PIPE, which is_open() reports.
        class STDC_EXPORT Stream : public std::iostream {
        public:
            Stream();
            ~Stream() override;

            /// Closes this end. Doing it twice is harmless.
            ///
            /// \note On the child's stdin this is what signals end of input, without which a
            ///       child reading to EOF never finishes.
            void close();

            bool is_open() const;

            /// The same pipe as a \c FILE *, for the C interfaces that take nothing else.
            ///
            /// \warning Owned by the Stream. Do not \c fclose it, and do not keep it past
            ///          close(), which leaves it dangling.
            FILE *file() const;

        private:
            friend class Popen;
            void open(FILE *file);

            class Buf;
            std::unique_ptr<Buf> _buf;

            STDC_DISABLE_COPY_MOVE(Stream)
        };

        Popen();
        ~Popen();

        /// Moving takes the child, its pipes and its settings across. Assigning over a Popen
        /// whose child is still running ends that child first.
        ///
        /// \note A Popen that has been moved from holds nothing and is only good for being
        ///       destroyed or assigned to.
        Popen(Popen &&RHS) noexcept;
        Popen &operator=(Popen &&RHS) noexcept;

    public:
        /// \name Setup
        ///
        /// All of these take effect at start() and mean nothing after it.
        /// @{

        /// The file to load, where that should not also be the name the program is given.
        ///
        /// \warning Almost nobody wants this. \c args()[0] is both the file to run and the name,
        ///          and leaving them the same is what nearly every program expects. It is not
        ///          what makes a path with a space in it work, nor what stops one being looked
        ///          up along \c PATH, since \c args()[0] covers both already.
        ///
        /// Set it only where the two must differ, which means a program that reads its own name
        /// and behaves accordingly. \c execve takes the file and the argument vector separately,
        /// so nothing requires them to agree, and \c login relies on that to start a shell under
        /// the name \c -bash.
        ///
        /// \code
        ///   // loads /bin/busybox, which finds "ls" as its name and behaves as ls
        ///   popen.executable("/bin/busybox").args({"ls", "-l"});
        /// \endcode
        ///
        /// \note Under shell() it names the shell instead, standing in for \c /bin/sh or
        ///       \c cmd.exe, and there it is the ordinary way to ask for a different one.
        /// \sa args()
        Popen &executable(std::filesystem::path executable);

        /// The argument vector, \c argv[0] included.
        ///
        /// \c args[0] is both the file to run and the name the program is given, unless
        /// executable() separates them. A name with no separator in it is looked up along
        /// \c PATH, and one with a separator is taken as written.
        ///
        /// \note Quoting is handled here. An argument with a space in it, \c args[0] included,
        ///       arrives at the program as one argument on either platform.
        /// \sa executable()
        Popen &args(std::vector<std::string> args);

        /// Whether \a args is short enough for the system to start a program with.
        ///
        /// Windows builds one string for \c CreateProcess and refuses it past 32767 characters,
        /// so this quotes \a args the way start() would and measures what comes out rather than
        /// guessing at it. POSIX counts the arguments and the environment together against
        /// \c ARG_MAX, so half of it is left for the environment, and no single argument may
        /// reach the 128 KiB one of its own that Linux imposes.
        ///
        /// This is what a response file is for. A build system generating one long command line
        /// asks this first, and writes the arguments to a file and passes \c \@file instead when
        /// the answer is no.
        ///
        /// \note What Windows counts is the UTF-16 these become, and UTF-8 is never shorter
        ///       than the UTF-16 of the same text, so counting bytes here can only refuse a
        ///       line the system would have taken. It never accepts one the system would
        ///       refuse, which is the direction that matters.
        /// \note A yes is not a promise that start() will succeed, only that it will not fail
        ///       for this reason. Both limits are approached conservatively.
        /// \sa cli::Parser::EnableResponseFile, which is the other end of the same problem
        static bool commandLineFits(const std::vector<std::string> &args);

        /// Hands the command to the system shell rather than executing it directly, so its
        /// redirections and expansions apply.
        ///
        /// \warning The shell's quoting rules apply too, so untrusted input reaching \a shell is
        ///          a command injection.
        Popen &shell(bool shell);

        /// The child's working directory. Inherited if left unset.
        Popen &cwd(std::filesystem::path cwd);

        /// The child's environment, which replaces ours rather than adding to it.
        ///
        /// \param env the variables to give the child, or \c std::nullopt to hand down the ones
        ///        this process has. An empty map asks for an empty environment, which is not the
        ///        same thing.
        /// \note Replacing it drops \c PATH along with everything else, so a bare program name
        ///       will not be found unless \a env carries one.
        Popen &env(std::optional<std::map<std::string, std::string>> env);

        // @overload: env(initializer_list)
        inline Popen &env(std::initializer_list<std::pair<const std::string, std::string>> env) {
            return this->env(std::map<std::string, std::string>(env));
        }

        /// Where each standard stream goes. Inherited if left unset.
        ///
        /// \param dev \c PIPE to talk over, \c DEVNULL to discard, a descriptor or \c FILE * to
        ///        hand it somewhere of your own, or \c STDOUT on stderr_() alone to fold the two
        ///        together
        Popen &stdin_(IODev dev);
        Popen &stdout_(IODev dev);
        Popen &stderr_(IODev dev);

        /// Opens the pipes in text mode, which on Windows translates between \c CRLF and \c LF
        /// as they are read and written. Nothing changes elsewhere.
        Popen &text(bool text);

        /// Whether the child starts with only the standard streams open. On by default, so a
        /// descriptor of ours is not left in a process that never asked for it.
        ///
        /// \sa pass_fds(), for the exceptions
        Popen &close_fds(bool close_fds);

        /// Starts the child independently of this object. Off by default, so destroying a Popen
        /// whose child is still running kills it.
        ///
        /// On Unix this uses \c setsid() and a double fork, leaving the final process to init or
        /// the nearest child subreaper. On Windows its process handle is closed after creation.
        /// In both cases pid() remains available, but wait(), poll(), communicate(), kill() and
        /// terminate() and send_signal() do not: this process no longer owns the child.
        ///
        /// \note Set this before start(). Changing it afterwards has no effect.
        /// \note \c PIPE is not supported for a detached child. Use inherited streams, files or
        ///       the null device.
        Popen &detached(bool detached);

        /// The capacity of the pipes created for this child, in bytes. The kernel rounds up,
        /// and caps it at \c /proc/sys/fs/pipe-max-size for an unprivileged caller.
        Popen &pipesize(int pipesize); // linux only (ignored on other platforms)

#ifdef _WIN32
        /// The \c STARTUPINFO fields to start the child with, and the attributes to give it.
        ///
        /// \param startupinfo what to pass \c CreateProcess, or \c std::nullopt to let this
        ///        decide, which is what the stream settings above already do
        /// \note Windows only, and taken by value: what is given here is copied rather than kept
        ///       as a reference to the caller's object.
        /// \note An \c lpAttributeList carrying \c handle_list decides for itself which handles
        ///       the child inherits, so it overrides close_fds() and says so in a warning.
        Popen &startupinfo(std::optional<StartupInfo> startupinfo);
        Popen &creationflags(int creationflags); // windows only
#else
        /// Runs in the child after the pipes are in place and before exec.
        ///
        /// \warning The child has one thread, the one that called \c fork. Any lock another
        ///          thread held at that moment is still held and will never be released, so
        ///          allocating or locking here can deadlock the child outright.
        Popen &preexec_fn(std::function<void()> preexec_fn); // unix only

        /// Puts the signal dispositions this process changed back to their defaults, so the
        /// child does not inherit an ignored \c SIGPIPE it never asked for. On by default.
        Popen &restore_signals(bool restore_signals); // unix only

        /// Runs \c setsid() in the child, putting it in a session of its own so a terminal
        /// signal aimed at this process group misses it.
        Popen &start_new_session(bool start_new_session); // unix only

        /// Descriptors to leave open across exec despite close_fds().
        ///
        /// \note Setting this forces close_fds() on, since the two disagree otherwise.
        Popen &pass_fds(std::vector<int> pass_fds); // unix only

        /// Credentials for the child.
        ///
        /// \pre The calling process is privileged. start() fails with \c EPERM otherwise.
        /// \note extra_groups() replaces the supplementary group list rather than adding to it.
        Popen &group(int group);                            // unix only
        Popen &extra_groups(std::vector<int> extra_groups); // unix only
        Popen &user(int user);                              // unix only
        /// The name is copied and need not outlive this call.
        Popen &user(const char *user); // unix only

        /// The file creation mask for the child, or -1 to inherit ours.
        Popen &umask(int umask); // unix only

        /// The process group to join, or 0 to start one of its own. -1 inherits ours.
        Popen &process_group(int process_group); // unix only
#endif

        /// @}

    public:
        /// \name Starting
        /// @{

        /// Starts the process.
        ///
        /// \param err_msg filled in with a readable description when this fails, if not null
        /// \retval true the child is running, and any \c PIPE stream is open
        /// \retval false nothing was started, with the reason in error_code()
        /// \note One Popen runs one child. Calling this again after a child has been started is
        ///       not supported. Use another Popen.
        bool start(std::string *err_msg = nullptr);

        /// The error from the last operation, cleared at the start of each one.
        std::error_code error_code() const;

        /// @}

    public:
        /// \name Waiting
        /// @{

        /// Whether the child has exited, without waiting for it.
        ///
        /// \retval true it has exited, and returncode() now holds the status
        /// \retval false it is still running, which is not an error, or the check itself failed
        /// \note Tell those two apart by returncode(), or by error_code() being clear.
        bool poll();

        /// Waits for the child to exit.
        ///
        /// \param timeout how long to wait, in milliseconds, or negative to wait forever
        /// \retval false the timeout ran out, or the wait failed
        /// \note The pipes stay readable afterwards, so output can still be collected.
        bool wait(int timeout = -1);

        /// Writes \a input to the child, reads stdout and stderr to the end, and waits.
        ///
        /// The only safe way to do all three, since draining one pipe at a time deadlocks as
        /// soon as the other one fills.
        ///
        /// \param input written to the child's stdin, which is then closed so that a child
        ///        reading to end of input can finish
        /// \param timeout how long to allow for writing, reading and waiting together, in
        ///        milliseconds, or negative for no limit
        /// \return what the child wrote to stdout and to stderr, each empty if that stream was
        ///         not a \c PIPE
        /// \note A child still running at \a timeout is killed rather than left behind, and
        ///       error_code() then reports a timeout.
        std::tuple<std::string, std::string> communicate(const std::string &input = {},
                                                         int timeout = -1);

        /// Sends \a sig to the child. On Windows only \c WS_CTRL_C_EVENT and
        /// \c WS_CTRL_BREAK_EVENT are accepted.
        bool send_signal(int sig);

        /// Requests the process to close, like \c QProcess::terminate. Posts \c WM_CLOSE to its
        /// windows on Windows and sends \c SIGTERM elsewhere.
        ///
        /// \note This is a request. The process may ignore it, and a console program has no
        ///       message loop to see it in the first place.
        /// \sa kill(), to force it
        bool terminate();

        /// Ends the process outright, which it cannot refuse.
        ///
        /// \warning Anything the child was part way through writing is lost.
        bool kill();

        /// @}

    public:
        /// \name Properties
        /// @{

        /// What executable() was set to, empty where it was not set.
        ///
        /// Not \c args()[0] in that case, on purpose. Which file runs is worked out by start(),
        /// along \c PATH where \c args()[0] carries no separator, so answering with it here would
        /// look like a resolution that has not happened yet.
        const std::filesystem::path &executable() const;

        array_view<std::string> args() const;

        /// The pipe for that stream. Not open unless it was set to \c PIPE.
        ///
        /// \warning These reference storage inside the Popen, so they do not outlive it.
        Stream &stdin_();
        const Stream &stdin_() const;
        Stream &stdout_();
        const Stream &stdout_() const;
        Stream &stderr_();
        const Stream &stderr_() const;

        int pid() const;

        bool detached() const;

        /// The exit status, or nothing while the child is still running.
        ///
        /// \note A child killed by a signal reports the negated signal number, as in Python, so
        ///       a \c SIGKILL comes back as -9.
        std::optional<int> returncode() const;

        /// @}

    protected:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    /// @}
}

#endif // STDCORELIB_POPEN_H

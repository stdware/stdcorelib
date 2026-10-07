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
/// Creation of child processes and loading of shared libraries on Windows and POSIX systems.
///
/// stdc::Popen is a port of Python's \c subprocess.Popen. The pipes are \c std::iostream objects
/// and support the standard stream operations. If more than one pipe is open, the pipes must be
/// read through stdc::Popen::communicate(). Reading the pipes one at a time deadlocks as soon as
/// the pipe that is not being read becomes full.
///
/// \code
///     using namespace stdc;
///
///     Popen proc;
///     proc.args({"git", "describe", "--tags"})
///         .standardInput(Popen::DeviceNull)
///         .standardOutput(Popen::Pipe)
///         .standardError(Popen::StandardOutput);
///
///     if (!proc.start()) {
///         return proc.errorMessage();
///     }
///     auto output = proc.communicate({}, 5000);
///     if (!output && proc.errorCode() == std::errc::timed_out) {
///         // The child is still running, and the output read so far is retained.
///         proc.kill();
///         output = proc.communicate();
///     }
///     if (!output) {
///         return proc.errorMessage();
///     }
///     auto &[out, _] = *output;
///     int code = proc.returnCode().value_or(-1);
/// \endcode
///
/// A Popen owns its child process and kills the child on destruction. With \c detached(true),
/// the child runs independently, and the Popen records only its process ID.
///
/// stdc::SharedLibrary loads a shared library at run time and resolves its symbols.
///
/// \code
///     SharedLibrary lib;
///     if (!lib.open(plugin_path)) {
///         return lib.errorMessage();
///     }
///     auto entry = reinterpret_cast<int (*)()>(lib.resolve("plugin_init"));
/// \endcode

namespace stdc {

    /// \addtogroup process
    /// @{

    /// A child process and its pipes, modeled on Python's \c subprocess.Popen.
    ///
    /// The setters return \c *this and can therefore be chained. The setters only record the
    /// configuration, and start() creates the process. After a successful start(), the child is
    /// running and every stream set to \c Pipe is open.
    ///
    /// \code
    ///   Popen proc;
    ///   proc.args({"git", "--version"})
    ///       .standardInput(Popen::DeviceNull)
    ///       .standardOutput(Popen::Pipe)
    ///       .standardError(Popen::StandardOutput); // redirects stderr into the stdout pipe
    ///
    ///   if (!proc.start()) {
    ///       return proc.errorMessage();
    ///   }
    ///   auto output = proc.communicate();
    ///   if (!output) {
    ///       return proc.errorMessage();
    ///   }
    ///   auto &[out, _] = *output;
    ///   int code = proc.returnCode().value_or(-1);
    /// \endcode
    ///
    /// Every \c std::string in this class is UTF-8, as in the rest of this library. The rule
    /// applies to args(), env(), the input of communicate() and the output that communicate()
    /// returns. On Windows, these strings are converted to UTF-16 before they are passed to
    /// \c CreateProcess. An argument in any writing system is therefore passed to the child
    /// without conversion to the system code page. executable() and cwd() accept
    /// \c std::filesystem::path values, which carry their own encoding and are passed unchanged.
    ///
    /// \note The structure of this class follows Python, and the names follow the conventions of
    ///       this library. For example, \c preexec_fn is preExec(), \c returncode is
    ///       returnCode(), \c stdin is standardInput(), and \c DEVNULL is \c DeviceNull. The
    ///       Python documentation describes the behavior of each setting.
    ///
    /// \warning A single pipe can be read directly, but two pipes must be read through
    ///          communicate(). A full pipe blocks its writer. If the child fills stderr while the
    ///          parent is still reading stdout, both processes wait indefinitely.
    ///
    /// \sa https://docs.python.org/3/library/subprocess.html
    class STDC_EXPORT Popen {
    public:
        /// The predefined targets of a standard stream, as alternatives to a file descriptor or
        /// a \c FILE * of the caller.
        enum IOType {
            Pipe = 1,       ///< a new pipe, accessible through the corresponding Stream
            DeviceNull,     ///< the null device
            StandardOutput, ///< the target of stdout, valid only for standardError()
        };

        struct IODev {
            enum Kind {
                None,
                Builtin,
                FileDescriptor,
                CFile,
            };
            IODev() : kind(None) {
            }
            IODev(IOType builtin) : kind(Builtin) {
                data.builtin = builtin;
            }
            IODev(int fd) : kind(FileDescriptor) {
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
            // Members of the Windows STARTUPINFO structure
            uint32_t dwFlags;
            void *hStdInput;
            void *hStdOutput;
            void *hStdError;
            uint16_t wShowWindow;

            // Supported keys:
            //     handle_list: the handles to be inherited, terminated by INVALID_HANDLE_VALUE
            std::map<std::string, void *> lpAttributeList;
        };

        enum WindowsSignal {
            WS_CTRL_C_EVENT = 0,
            WS_CTRL_BREAK_EVENT = 1,
        };
#endif

        /// The end of a pipe to the child that belongs to this process, as a \c std::iostream.
        ///
        /// A Stream is open only if the corresponding standard stream of the child is set to
        /// \c Pipe.
        class STDC_EXPORT Stream : public std::iostream {
        public:
            Stream();
            ~Stream() override;

            /// Closes this end of the pipe. Closing a closed Stream has no effect.
            ///
            /// \note Closing the stdin pipe signals the end of input to the child. A child that
            ///       reads until the end of input does not finish before this end is closed.
            void close();

            bool isOpen() const;

            /// Returns the same pipe as a \c FILE *, for C interfaces that accept only a
            /// \c FILE *.
            ///
            /// \warning The Stream owns the \c FILE. The caller must not call \c fclose on it and
            ///          must not use it after close(), which invalidates the pointer.
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

        /// Transfers the child, its pipes and its configuration. Move assignment destroys the
        /// previous state of the destination immediately. A running child owned by that state is
        /// killed and waited for at that point. A detached child continues to run.
        ///
        /// \note A moved-from Popen holds no state and may only be destroyed or assigned to.
        Popen(Popen &&RHS) noexcept;
        Popen &operator=(Popen &&RHS) noexcept;

    public:
        /// \name Setup
        ///
        /// These settings take effect at start() and have no effect after it.
        /// @{

        /// Sets the file to load if the file must differ from the program name that the child
        /// receives.
        ///
        /// \warning This setting is rarely required. \c args()[0] specifies both the file to run
        ///          and the program name, and nearly every program expects the two to be
        ///          identical. The setting is not required for a path that contains a space or
        ///          to prevent a lookup along \c PATH, because \c args()[0] already handles both
        ///          cases.
        ///
        /// The setting is intended for a program that reads its own name and behaves
        /// accordingly. \c execve accepts the file and the argument vector separately and does
        /// not require them to agree. \c login relies on this behavior to start a shell under the
        /// name \c -bash.
        ///
        /// \code
        ///   // loads /bin/busybox, which runs as ls because its program name is "ls"
        ///   popen.executable("/bin/busybox").args({"ls", "-l"});
        /// \endcode
        ///
        /// \note If shell() is enabled, the setting specifies the shell in place of \c /bin/sh or
        ///       \c cmd.exe. This is the standard method to select a different shell.
        /// \sa args()
        Popen &executable(std::filesystem::path executable);

        /// Sets the argument vector, including \c argv[0].
        ///
        /// \c args[0] specifies both the file to run and the program name, unless executable()
        /// specifies the file separately. A name without a separator is looked up along \c PATH,
        /// and a name with a separator is used as written.
        ///
        /// \note This class quotes the arguments. An argument that contains a space, including
        ///       \c args[0], reaches the program as one argument on every platform.
        /// \sa executable()
        Popen &args(std::vector<std::string> args);

        /// Sets whether the command runs through the system shell rather than directly.
        ///
        /// args() retains its meaning as an argument vector if this setting is enabled. Each
        /// element is quoted for the shell of the platform, so that spaces, quotation marks and
        /// shell metacharacters remain part of that element. Redirections and expansions
        /// therefore do not apply to the arguments.
        ///
        /// \note On Windows, the console window of the shell is hidden unless startupInfo() sets
        ///       \c STARTF_USESHOWWINDOW in \c dwFlags. In that case, \c wShowWindow determines
        ///       the window state.
        Popen &shell(bool shell);

        /// Sets the working directory of the child. If unset, the child inherits the working
        /// directory of this process.
        Popen &cwd(std::filesystem::path cwd);

        /// Sets the environment of the child, which replaces the environment of this process
        /// rather than extending it.
        ///
        /// \param env the variables of the child, or \c std::nullopt to pass the environment of
        ///        this process. An empty map specifies an empty environment, which differs from
        ///        \c std::nullopt.
        /// \note On POSIX systems, a program name without a separator is looked up along the
        ///       \c PATH of \a env. If \a env contains no \c PATH, the name is not found.
        Popen &env(std::optional<std::map<std::string, std::string>> env);

        /// \overload
        inline Popen &env(std::initializer_list<std::pair<const std::string, std::string>> env) {
            return this->env(std::map<std::string, std::string>(env));
        }

        /// Sets the target of each standard stream. If unset, the stream is inherited.
        ///
        /// \param dev \c Pipe for a pipe to this process, \c DeviceNull to discard the stream, a
        ///        file descriptor or \c FILE * of the caller, or \c StandardOutput, which is
        ///        valid only for standardError(), to merge stderr into stdout
        Popen &standardInput(IODev dev);
        Popen &standardOutput(IODev dev);
        Popen &standardError(IODev dev);

        /// Sets whether the pipes are opened in text mode. On Windows, text mode translates
        /// between \c CRLF and \c LF during reading and writing. On other platforms, the setting
        /// has no effect.
        Popen &text(bool text);

        /// Sets whether the child starts with only the standard streams open. The setting is
        /// enabled by default, so that the descriptors of this process do not leak into the
        /// child. passFds() specifies the exceptions.
        ///
        /// \sa passFds()
        Popen &closeFds(bool closeFds);

        /// Sets whether the child starts independently of this object. The setting is disabled
        /// by default, and destroying a Popen whose child is still running kills the child.
        ///
        /// On Unix, the setting uses \c setsid() and a double fork, and init or the nearest
        /// child subreaper adopts the final process. On Windows, the process handle is closed
        /// after creation. In both cases, pid() remains available, but wait(), poll(),
        /// communicate(), kill(), terminate() and sendSignal() fail, because this process no
        /// longer owns the child.
        ///
        /// \note The setting must be made before start(). A change after start() has no effect.
        /// \note \c Pipe is not supported for a detached child. Inherited streams, files and the
        ///       null device are supported.
        Popen &detached(bool detached);

        /// Sets the capacity in bytes of the pipes created for the child. The kernel rounds the
        /// value up and limits it to \c /proc/sys/fs/pipe-max-size for an unprivileged process.
        Popen &pipeSize(int pipeSize); // linux only (ignored on other platforms)

#ifdef _WIN32
        /// Sets the \c STARTUPINFO fields and the attributes with which the child is created.
        ///
        /// \param startupInfo the values to pass to \c CreateProcess, or \c std::nullopt to
        ///        derive them from the stream settings
        /// \note The value is copied. The object of the caller is not referenced after the call.
        /// \note If \c lpAttributeList contains \c handle_list, that list determines the handles
        ///       that the child inherits. The list therefore overrides closeFds(), and start()
        ///       writes a warning to stderr.
        /// \note If \c dwFlags contains \c STARTF_USESHOWWINDOW, \c wShowWindow overrides the
        ///       hidden console window of shell().
        Popen &startupInfo(std::optional<StartupInfo> startupInfo);
        Popen &creationFlags(int creationFlags); // windows only
#else
        /// Sets a function that runs in the child after the pipes are connected and before
        /// \c exec.
        ///
        /// \warning The child contains only the thread that called \c fork. A lock that another
        ///          thread held at the time of the fork remains held and is never released.
        ///          Memory allocation or locking in this function can therefore deadlock the
        ///          child.
        Popen &preExec(std::function<void()> preExec); // unix only

        /// Sets whether the child resets \c SIGPIPE and \c SIGXFSZ to their default
        /// dispositions, as Python does. The setting is enabled by default, so that the child
        /// does not inherit an ignored \c SIGPIPE.
        Popen &restoreSignals(bool restoreSignals); // unix only

        /// Sets whether the child calls \c setsid(), which places the child in a new session. A
        /// terminal signal sent to the process group of this process then does not reach the
        /// child.
        Popen &startNewSession(bool startNewSession); // unix only

        /// Sets the descriptors that remain open across \c exec despite closeFds().
        ///
        /// \note A non-empty list enables closeFds(), because the list is meaningful only if
        ///       closeFds() is enabled.
        Popen &passFds(std::vector<int> passFds); // unix only

        /// Sets the credentials of the child.
        ///
        /// \pre The calling process is privileged. Otherwise, start() fails with \c EPERM.
        /// \note extraGroups() replaces the supplementary group list rather than extending it.
        Popen &group(int group);                          // unix only
        Popen &extraGroups(std::vector<int> extraGroups); // unix only
        Popen &user(int user);                            // unix only
        /// Copies the name, which is therefore not required to outlive the call.
        Popen &user(const char *user); // unix only

        /// Sets the file creation mask of the child, or -1 to inherit the mask of this process.
        Popen &umask(int umask); // unix only

        /// Sets the process group that the child joins, or 0 for a new group led by the child.
        /// The value -1 keeps the process group of this process.
        Popen &processGroup(int processGroup); // unix only
#endif

        /// @}

    public:
        /// \name Starting
        /// @{

        /// Starts the child process.
        ///
        /// \retval true the child is running, and every \c Pipe stream is open
        /// \retval false no process was started, and errorMessage() describes the cause
        /// \note After a failed start, errorMessage() is the authoritative description. Many
        ///       causes of failure concern the configuration rather than a system call, and no
        ///       specific error code represents those causes.
        /// \note One Popen runs one child. A second call after a child has been started is not
        ///       supported, and a new Popen is required.
        bool start();

        /// @}

    public:
        /// \name Waiting
        ///
        /// These functions fail with \c operation_not_supported for a detached child, which this
        /// process no longer owns.
        /// @{

        /// Returns whether the child has exited, without waiting for the child.
        ///
        /// \retval true the child has exited, and returnCode() contains the exit status
        /// \retval false the child is still running, or the check failed
        /// \note errorCode() is clear if the child is still running and set if the check failed.
        bool poll();

        /// Waits for the child to exit.
        ///
        /// \param timeout the time limit in milliseconds, or a negative value for no limit
        /// \retval true the child has exited
        /// \retval false the time limit expired, or the wait failed
        /// \note The pipes remain readable after the wait, and the output can still be collected.
        bool wait(int timeout = -1);

        /// Writes \a input to the stdin of the child, reads stdout and stderr until the end of
        /// file, and waits for the child to exit.
        ///
        /// This function is the only safe method to perform the three operations, because
        /// reading the pipes one at a time deadlocks as soon as the pipe that is not being read
        /// becomes full.
        ///
        /// \param input the data written to stdin, which is then closed so that a child that
        ///        reads until the end of input can finish. Only the first call may pass a
        ///        non-empty \a input. A later call with a non-empty \a input fails with
        ///        \c invalid_argument.
        /// \param timeout the time limit in milliseconds for writing, reading and waiting
        ///        together, or a negative value for no limit
        /// \return the output of the child on stdout and stderr, each empty if the stream is not
        ///         a \c Pipe, or \c std::nullopt if the exchange did not complete, with the cause
        ///         in errorCode()
        /// \note A child that is still running at the time limit is left running, as in Python,
        ///       and errorCode() reports \c timed_out. A subsequent call resumes the exchange,
        ///       and the output read so far is retained. To abandon the child, the caller calls
        ///       kill() and then calls this function again to collect the remaining output.
        /// \warning The pipes are reserved for this function until a call returns the output.
        ///          The caller must not read, write or close the streams in between.
        std::optional<std::tuple<std::string, std::string>>
            communicate(const std::string &input = {}, int timeout = -1);

        /// Sends \a sig to the child. On Windows, only \c WS_CTRL_C_EVENT and
        /// \c WS_CTRL_BREAK_EVENT are accepted.
        ///
        /// \note The function rejects a detached child although pid() identifies it. A process
        ///       ID is safe to signal only while the process is known to be running, because the
        ///       system may reuse the ID as soon as the process exits. The ID of an owned child
        ///       remains reserved until the child is waited for, and this function checks the
        ///       state of the child first. A detached child cannot be waited for, and its state
        ///       therefore cannot be checked. A caller with other evidence that the child is
        ///       still running can send the signal through pid().
        bool sendSignal(int sig);

        /// Requests the process to close, like \c QProcess::terminate(). On Windows, the
        /// function posts \c WM_CLOSE to the windows of the process. On other platforms, the
        /// function sends \c SIGTERM.
        ///
        /// The process can ignore the request, and a console program has no message loop that
        /// receives \c WM_CLOSE. kill() forces termination. sendSignal() describes the reason
        /// for which a detached child is rejected.
        ///
        /// \sa kill(), sendSignal()
        bool terminate();

        /// Terminates the process immediately. The process cannot ignore the termination.
        ///
        /// sendSignal() describes the reason for which a detached child is rejected.
        ///
        /// \warning Data that the child was in the middle of writing is lost.
        /// \sa sendSignal()
        bool kill();

        /// @}

    public:
        /// \name Failures
        ///
        /// Both functions describe the most recent operation and are cleared at the start of the
        /// next operation.
        /// @{

        /// Returns the failure as an error code.
        std::error_code errorCode() const;

        /// Returns the same failure as text, or an empty string if the most recent operation
        /// succeeded.
        ///
        /// \note The text is more specific than errorCode().message() if the failure occurred
        ///       in a system call, because the text names the call. The text also describes a
        ///       request that was rejected before any system call, which no \c errno value
        ///       describes.
        std::string errorMessage() const;

        /// @}

    public:
        /// \name Properties
        /// @{

        /// Returns the value set by executable(), or an empty path if the value is not set.
        ///
        /// If the value is not set, the function does not return \c args()[0]. start() resolves
        /// the file to run, along \c PATH if \c args()[0] contains no separator. Returning
        /// \c args()[0] would therefore suggest a resolution that has not yet taken place.
        const std::filesystem::path &executable() const;

        array_view<std::string> args() const;

        /// Returns the pipe of the corresponding stream, which is open only if the stream is set
        /// to \c Pipe.
        ///
        /// \warning The streams are stored inside the Popen and do not outlive it.
        Stream &standardInput() const;
        Stream &standardOutput() const;
        Stream &standardError() const;

        /// Returns the process ID of the child, or -1 before start() and after a failed start().
        ///
        /// \note For a detached child, the ID identifies the process that runs the program, not
        ///       the intermediate process that forked it. The intermediate process exits before
        ///       start() returns.
        /// \note The ID remains available after the child exits, and the system may reuse it
        ///       once the child has been waited for. The ID therefore identifies a child of this
        ///       process only while returnCode() is empty.
        int pid() const;

        /// Returns the value set by detached(bool). start() applies the value at the time of
        /// the call, and a later change has no effect.
        bool detached() const;

        /// Returns the exit status, or \c std::nullopt while the child is still running.
        ///
        /// \note A child terminated by a signal reports the negated signal number, as in Python.
        ///       A \c SIGKILL therefore results in -9.
        std::optional<int> returnCode() const;

        /// @}

    protected:
        class Impl;
        std::unique_ptr<Impl> _impl;
    };

    /// @}
}

#endif // STDCORELIB_POPEN_H

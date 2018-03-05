/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <sys/types.h>
#include <sys/wait.h>
#include <cassert>
#include <cstdlib>
#include <sys/syscall.h>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "child_process.hh"
#include "system_runner.hh"
#include "exception.hh"
#include "signalfd.hh"

using namespace std;
using namespace PollerShortNames;

template <typename T> void zero( T & x ) { memset( &x, 0, sizeof( x ) ); }

int do_fork()
{
    /* Verify that process is single-threaded before forking */
    {
        struct stat my_stat;
        CheckSystemCall( "stat", stat( "/proc/self/task", &my_stat ) );

        if ( my_stat.st_nlink != 3 ) {
            throw runtime_error( "ChildProcess constructed in multi-threaded program" );
        }
    }

    return CheckSystemCall( "fork", fork() );
}

/* start up a child process running the supplied lambda */
/* the return value of the lambda is the child's exit status */
ChildProcess::ChildProcess( const string & name,
                            function<int()> && child_procedure,
                            const int termination_signal )
    : name_( name ),
      pid_( do_fork() ),
      running_( true ),
      terminated_( false ),
      exit_status_(),
      died_on_signal_( false ),
      graceful_termination_signal_( termination_signal ),
      moved_away_( false )
{
    if ( pid_ == 0 ) { /* child */
        try {
            SignalMask( {} ).set_as_mask();
            _exit( child_procedure() );
        } catch ( const exception & e ) {
            print_exception( name_.c_str(), e );
            _exit( EXIT_FAILURE );
        }
    }
}

/* is process in a waitable state? */
bool ChildProcess::waitable( void ) const
{
    assert( !moved_away_ );
    assert( !terminated_ );

    siginfo_t infop;
    zero( infop );
    CheckSystemCall( "waitid", waitid( P_PID, pid_, &infop,
                                       WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT ) );

    if ( infop.si_pid == 0 ) {
        return false;
    } else if ( infop.si_pid == pid_ ) {
        return true;
    } else {
        throw runtime_error( "waitid: unexpected value in siginfo_t si_pid field (not 0 or pid)" );
    }
}

/* wait for process to change state */
void ChildProcess::wait( const bool nonblocking )
{
    assert( !moved_away_ );
    assert( !terminated_ );

    siginfo_t infop;
    zero( infop );
    CheckSystemCall( "waitid", waitid( P_PID, pid_, &infop,
                                       WEXITED | WSTOPPED | WCONTINUED | (nonblocking ? WNOHANG : 0) ) );

    if ( nonblocking and (infop.si_pid == 0) ) {
        throw runtime_error( "nonblocking wait: process was not waitable" );
    }

    if ( infop.si_pid != pid_ ) {
        throw runtime_error( "waitid: unexpected value in siginfo_t si_pid field" );
    }

    if ( infop.si_signo != SIGCHLD ) {
        throw runtime_error( "waitid: unexpected value in siginfo_t si_signo field (not SIGCHLD)" );
    }

    /* how did the process change state? */
    switch ( infop.si_code ) {
    case CLD_EXITED:
        terminated_ = true;
        exit_status_ = infop.si_status;
        break;
    case CLD_KILLED:
    case CLD_DUMPED:
        terminated_ = true;
        exit_status_ = infop.si_status;
        died_on_signal_ = true;
        break;
    case CLD_STOPPED:
        running_ = false;
        break;
    case CLD_CONTINUED:
        running_ = true;
        break;
    default:
        throw runtime_error( "waitid: unexpected siginfo_t si_code" );
    }
}

/* if child process was suspended, resume it */
void ChildProcess::resume( void )
{
    assert( !moved_away_ );

    if ( !running_ ) {
        signal( SIGCONT );
    }
}

/* send a signal to the child process */
void ChildProcess::signal( const int sig )
{
    assert( !moved_away_ );

    if ( !terminated_ ) {
        CheckSystemCall( "kill", kill( pid_, sig ) );
    }
}

ChildProcess::~ChildProcess()
{
    if ( moved_away_ ) { return; }

    try {
        while ( !terminated_ ) {
            resume();
            signal( graceful_termination_signal_ );
            wait();
        }
    } catch ( const exception & e ) {
        print_exception( name_.c_str(), e );
    }

    cerr << "Process " << pid_ << " is terminated gracefully" << endl;
}

/* move constructor */
ChildProcess::ChildProcess( ChildProcess && other )
    : name_( other.name_ ),
      pid_( other.pid_ ),
      running_( other.running_ ),
      terminated_( other.terminated_ ),
      exit_status_( other.exit_status_ ),
      died_on_signal_( other.died_on_signal_ ),
      graceful_termination_signal_( other.graceful_termination_signal_ ),
      moved_away_( other.moved_away_ )
{
    assert( !other.moved_away_ );

    other.moved_away_ = true;
}

void ChildProcess::throw_exception( void ) const
{
    throw runtime_error( "`" + name() + "': process "
                         + (died_on_signal()
                            ? string("died on signal ")
                            : string("exited with failure status "))
                         + to_string( exit_status() ) );
}

ProcessManager::ProcessManager()
  : child_processes_(),
    poller_(),
    signals_({ SIGCHLD, SIGABRT, SIGHUP, SIGINT, SIGQUIT, SIGTERM }),
    signal_fd_(signals_)
{
  /* use signal_fd_ to read signals */
  signals_.set_as_mask();

  /* poller listens on signal_fd_ for signals */
  poller_.add_action(
    Poller::Action(signal_fd_.fd(), Direction::In,
      [&]() {
        return handle_signal(signal_fd_.read_signal());
      }
    )
  );
}

void ProcessManager::run_as_child(const string & program,
                                  const vector<string> & prog_args)
{
  auto child = ChildProcess(program,
    [&]() {
      return ezexec(program, prog_args);
    }
  );

  child_processes_.emplace(child.pid(), move(child));
}

int ProcessManager::wait()
{
  /* poll forever */
  for (;;) {
    const Poller::Result & ret = poller_.poll(-1);
    if (ret.result == Poller::Result::Type::Exit) {
      return ret.exit_status;
    }
  }
}

int ProcessManager::run(const string & program,
                         const vector<string> & prog_args)
{
  run_as_child(program, prog_args);
  return wait();
}

Result ProcessManager::handle_signal(const signalfd_siginfo & sig)
{
  switch (sig.ssi_signo) {
  case SIGCHLD:
    if (child_processes_.empty()) {
      cerr << "ProcessManager: received SIGCHLD without any children" << endl;
      return {ResultType::Exit, EXIT_FAILURE};
    }

    for (auto it = child_processes_.begin(); it != child_processes_.end();) {
      ChildProcess & child = it->second;

      if (not child.waitable()) {
        ++it;
      } else {
        child.wait(true);

        if (child.terminated()) {
          if (child.exit_status() != 0) {
            cerr << "ProcessManager: PID " << it->first
                 << " exits abnormally" << endl;
            return {ResultType::Exit, EXIT_FAILURE};
          }

          it = child_processes_.erase(it);
        } else {
          if (not child.running()) {
            cerr << "ProcessManager: PID " << it->first
                 << " is not running" << endl;
            return {ResultType::Exit, EXIT_FAILURE};
          }

          ++it;
        }
      }
    }

    break;
  case SIGABRT:
  case SIGHUP:
  case SIGINT:
  case SIGQUIT:
  case SIGTERM:
    cerr << "ProcessManager: interrupted by signal " << sig.ssi_signo << endl;
    return {ResultType::Exit, EXIT_FAILURE};
  default:
    cerr << "ProcessManager: unknown signal " << sig.ssi_signo << endl;
    return {ResultType::Exit, EXIT_FAILURE};
  }

  return {ResultType::Continue, EXIT_SUCCESS};
}

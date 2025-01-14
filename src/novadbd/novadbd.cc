/*
 * Flare
 * --------------
 * Copyright (C) 2008-2014 GREE, Inc.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
/**
 *	novadbd.cc
 *
 *	implementation of gree::flare::novadbd (and some other global stuffs)
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#include "novadbd.h"
#include "connection_tcp.h"
#include "handler_alarm.h"
#include "handler_request.h"
#ifdef ENABLE_MYSQL_REPLICATION
# include "handler_mysql_replication.h"
#endif
#include "show_node.h"
#include "time_watcher.h"
#include "time_watcher_observer.h"

#include "storage_tch.h"
#include "storage_tcb.h"
#ifdef HAVE_LIBKYOTOCABINET
#include "storage_kch.h"
#endif

namespace gree {
namespace flare {

// This variable is set by main thread.
static volatile sig_atomic_t reload_request = 0;

// {{{ global functions
/**
 *	signal handler (SIGTERM/SIGINT)
 */
void sa_term_handler(int sig) {
	if (sig != SIGTERM && sig != SIGINT) {
		return;
	}
	log_notice("received signal [%s] -> requesting shutdown", sig == SIGTERM ? "SIGTERM" : "SIGINT");

	singleton<novadbd>::instance().request_shutdown();

	return;
}

/**
 *	signal handler (SIGHUP)
 */
void sa_hup_handler(int sig) {
	//
	// Set signal flag. 
	// Reload action is executed in the main loop.
	//
	reload_request = 1;
}

/**
 *	signal handler (SIGUSR1)
 */
void sa_usr1_handler(int sig) {
	log_notice("received signal [SIGUSR1]", 0);

	// just interrupting -> nothing to do
}
// }}}

// {{{ ctor/dtor
/**
 *	ctor for novadbd
 */
novadbd::novadbd():
		_server(NULL),
		_thread_index(NULL),
		_req_thread_pool(NULL),
		_other_thread_pool(NULL),
		_cluster(NULL),
		_storage(NULL) {
}

/**
 *	dtor for novadbd
 */
novadbd::~novadbd() {
	delete this->_storage;
	this->_storage = NULL;

	delete this->_server;
	this->_server = NULL;

	delete this->_req_thread_pool;
	this->_req_thread_pool = NULL;

	delete this->_other_thread_pool;
	this->_other_thread_pool = NULL;

	delete this->_thread_index;
	this->_thread_index = NULL;

	delete this->_cluster;
	this->_cluster = NULL;

	delete stats_object;
	stats_object = NULL;

	delete status_object;
	status_object = NULL;

	delete time_watcher_object;
}
// }}}

// {{{ operator overloads
// }}}

// {{{ public methods
/**
 *	novadbd application startup procs
 */
int novadbd::startup(int argc, char **argv) {
	ini_option_object().set_args(argc, argv);
	if (ini_option_object().load() < 0) {
		return -1;
	}

	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility(), ini_option_object().get_log_stderr());
	stats_object = new stats_node();
	stats_object->startup();

	status_object = new status_node();

	log_notice("%s version %s - system logger started", this->_ident.c_str(), PACKAGE_VERSION);

	log_notice("application startup in progress...", 0);

	vector<string> lines = show_node::lines();
	for (vector<string>::iterator line = lines.begin(); line != lines.end(); line++) {
		log_notice("  %s", line->c_str());
	}

	// startup procs
	if (this->_set_resource_limit() < 0) {
		return -1;
	}

	if (ini_option_object().is_daemonize()) {
		if (this->_daemonize() < 0) {
			return -1;
		}
	}

	if (this->_set_signal_handler() < 0) {
		return -1;
	}

	// application objects
	connection_tcp::read_timeout = ini_option_object().get_net_read_timeout() * 1000;		// -> msec
	this->_server = new server();
	this->_server->set_back_log(ini_option_object().get_back_log());
	if (this->_server->listen(ini_option_object().get_server_port()) < 0) {
		return -1;
	}
	if (ini_option_object().get_server_socket().empty() == false) {
		if (this->_server->listen(ini_option_object().get_server_socket()) < 0) {
			return -1;
		}
	}

	this->_thread_index = new AtomicCounter(1);
	this->_req_thread_pool = new thread_pool(ini_option_object().get_thread_pool_size(), ini_option_object().get_stack_size(), this->_thread_index);
	this->_other_thread_pool = new thread_pool(ini_option_object().get_thread_pool_size(), ini_option_object().get_stack_size(), this->_thread_index);

	this->_cluster_replication = shared_cluster_replication(new cluster_replication(this->_other_thread_pool));

	this->_cluster = new cluster(this->_req_thread_pool, this->_other_thread_pool, ini_option_object().get_server_name(), ini_option_object().get_server_port());
	this->_cluster->set_proxy_concurrency(ini_option_object().get_proxy_concurrency());
	this->_cluster->set_reconstruction_interval(ini_option_object().get_reconstruction_interval());
	this->_cluster->set_reconstruction_bwlimit(ini_option_object().get_reconstruction_bwlimit());
	this->_cluster->set_replication_type(ini_option_object().get_replication_type());
	this->_cluster->set_max_total_thread_queue(ini_option_object().get_max_total_thread_queue());
	this->_cluster->set_noreply_window_limit(ini_option_object().get_noreply_window_limit());
	this->_cluster->add_proxy_event_listener(this->_cluster_replication);
	if (this->_cluster->startup_node(ini_option_object().get_index_servers(),
																	 ini_option_object().get_proxy_prior_netmask()) < 0) {
		return -1;
	}

	storage::type t = storage::type_tch;
	storage::type_cast(ini_option_object().get_storage_type(), t);
	switch (t) {
	case storage::type_tch:
		this->_storage = new storage_tch(ini_option_object().get_data_dir(),
				ini_option_object().get_mutex_slot(),
				ini_option_object().get_storage_ap(),
				ini_option_object().get_storage_fp(),
				ini_option_object().get_storage_bucket_size(),
				ini_option_object().get_storage_cache_size(),
				ini_option_object().get_storage_compress(),
				ini_option_object().is_storage_large(),
				ini_option_object().get_storage_dfunit());

		break;
	case storage::type_tcb:
		this->_storage = new storage_tcb(ini_option_object().get_data_dir(),
				ini_option_object().get_mutex_slot(),
				ini_option_object().get_storage_ap(),
				ini_option_object().get_storage_fp(),
				ini_option_object().get_storage_bucket_size(),
				ini_option_object().get_storage_cache_size(),
				ini_option_object().get_storage_compress(),
				ini_option_object().is_storage_large(),
				ini_option_object().get_storage_lmemb(),
				ini_option_object().get_storage_nmemb(),
				ini_option_object().get_storage_dfunit());
		break;
	#ifdef HAVE_LIBKYOTOCABINET
	case storage::type_kch:
		this->_storage = new storage_kch(ini_option_object().get_data_dir(),
				ini_option_object().get_mutex_slot(),
				ini_option_object().get_storage_ap(),
				ini_option_object().get_storage_bucket_size(),
				ini_option_object().get_storage_cache_size(),
				ini_option_object().get_storage_compress(),
				ini_option_object().is_storage_large(),
				ini_option_object().get_storage_dfunit());
		break;
	#endif
	default:
		log_err("unknown storage type [%s]", ini_option_object().get_storage_type().c_str());
		return -1;
	}
	if (this->_storage->open() < 0) {
		return -1;
	}
	this->_storage->set_listener(this);
	this->_cluster->set_storage(this->_storage);

	// creating alarm thread in advance
	shared_thread th_alarm = this->_other_thread_pool->get(thread_pool::thread_type_alarm);
	handler_alarm* h_alarm = new handler_alarm(th_alarm);
	th_alarm->trigger(h_alarm);

	time_watcher_object = new time_watcher();
	time_watcher_observer::set_threshold_warn_msec(ini_option_object().get_storage_access_watch_threshold_warn_msec());
	time_watcher_observer::set_threshold_ping_ng_msec(ini_option_object().get_storage_access_watch_threshold_ping_ng_msec());
	time_watcher_observer::set_storage_listener(this);
	if (ini_option_object().get_time_watcher_enabled()) {
		time_watcher_object->start(ini_option_object().get_time_watcher_polling_interval_msec());
	}


#ifdef ENABLE_MYSQL_REPLICATION
	if (ini_option_object().is_mysql_replication()) {
		shared_thread th_mysql_replication = this->_other_thread_pool->get(thread_pool::thread_type_mysql_replication);
		handler_mysql_replication* h_mysql_replication = new handler_mysql_replication(th_mysql_replication, this->_cluster);
		th_mysql_replication->trigger(h_mysql_replication);
	}
#endif

	// cluster replication
	cluster_replication::mode m = cluster_replication::mode_duplicate;
	if (cluster_replication::mode_cast(ini_option_object().get_cluster_replication_mode(), m) >= 0) {
		this->_cluster_replication->set_mode(m);
	} else {
		return -1;
	}
	if (ini_option_object().is_cluster_replication()) {
		string n = ini_option_object().get_cluster_replication_server_name();
		int p = ini_option_object().get_cluster_replication_server_port();
		int c = ini_option_object().get_cluster_replication_concurrency();
		if (this->_cluster_replication->start(n, p, c, this->_storage, this->_cluster) < 0) {
			return -1;
		}
	}

	if (this->_set_pid() < 0) {
		return -1;
	}

	return 0;
}

/**
 *	novadbd application running loop
 */
int novadbd::run() {
	log_notice("entering running loop", 0);

	for (;;) {
		if (this->_shutdown_request) {
			log_notice("shutdown request accepted -> breaking running loop", 0);
			log_notice("send shutdown message to index server", 0);
			this->_server->close(); /* prevent this node from responding */
			if (this->_cluster->shutdown_node()) {
				log_warning("failed to send shutdown message", 0);
			}
			break;
		}

		if (reload_request) {
			log_notice("received signal [SIGHUP] -> reloading", 0);
			singleton<novadbd>::instance().reload();
			reload_request = 0;
		}

		vector<shared_connection_tcp> connection_list = this->_server->wait();

		if (reload_request) {
			log_notice("received signal [SIGHUP] -> reloading", 0);
			singleton<novadbd>::instance().reload();
			reload_request = 0;
		}

		vector<shared_connection_tcp>::iterator it;
		for (it = connection_list.begin(); it != connection_list.end(); it++) {
			shared_connection_tcp c = *it;

			if (this->_req_thread_pool->get_thread_size(thread_pool::thread_type_request) >= ini_option_object().get_max_connection()) {
				log_warning("too many connections [%d] -> closing socket and continue", ini_option_object().get_max_connection());
				continue;
			}

			stats_object->increment_total_connections();

			shared_thread t = this->_req_thread_pool->get(thread_pool::thread_type_request);
			if (t->get_id() == 0) {
				log_warning("too many threads (failed to create thread) [%d] -> closing socket and continue", this->_req_thread_pool->get_thread_size(thread_pool::thread_type_request));
				continue;
			}
			handler_request* h = new handler_request(t, c);
			t->trigger(h);
		}
	}

	return 0;
}

/**
 *	novadbd application reload procs
 */
int novadbd::reload() {
	if (ini_option_object().reload() < 0) {
		log_notice("invalid config file -> skip reloading", 0);
		return -1;
	}

	// log_facility
	log_notice("re-opening syslog...", 0);
	singleton<logger>::instance().close();
	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility(), ini_option_object().get_log_stderr());

	// net_read_timeout
	connection_tcp::read_timeout = ini_option_object().get_net_read_timeout() * 1000;	// -> msec

	//  index_servers
	this->_cluster->set_index_servers(ini_option_object().get_index_servers());

	// reconstruction_interval
	this->_cluster->set_reconstruction_interval(ini_option_object().get_reconstruction_interval());

	// reconstruction_interval
	this->_cluster->set_reconstruction_interval(ini_option_object().get_reconstruction_interval());

	// reconstruction_bwlimit
	this->_cluster->set_reconstruction_bwlimit(ini_option_object().get_reconstruction_bwlimit());

	// replication_type
	this->_cluster->set_replication_type(ini_option_object().get_replication_type());
	
	// thread_pool_size
	this->_req_thread_pool->set_max_pool_size(ini_option_object().get_thread_pool_size());

	// thread_pool_size
	this->_other_thread_pool->set_max_pool_size(ini_option_object().get_thread_pool_size());

	// max_total_thread_queue
	this->_cluster->set_max_total_thread_queue(ini_option_object().get_max_total_thread_queue());

	// re-setup resource limit (do not care about return value here)
	this->_set_resource_limit();

	// noreply_window_limit
	this->_cluster->set_noreply_window_limit(ini_option_object().get_noreply_window_limit());

	time_watcher_object->stop();
	time_watcher_observer::set_threshold_warn_msec(ini_option_object().get_storage_access_watch_threshold_warn_msec());
	time_watcher_observer::set_threshold_ping_ng_msec(ini_option_object().get_storage_access_watch_threshold_ping_ng_msec());
	if (ini_option_object().get_time_watcher_enabled()) {
		time_watcher_object->start(ini_option_object().get_time_watcher_polling_interval_msec());
	}

	// cluster replication
	cluster_replication::mode m = cluster_replication::mode_duplicate;
	if (cluster_replication::mode_cast(ini_option_object().get_cluster_replication_mode(), m) >= 0) {
		this->_cluster_replication->set_mode(m);
		if (ini_option_object().is_cluster_replication()) {
			string cl_repl_server_name = ini_option_object().get_cluster_replication_server_name();
			int cl_repl_server_port = ini_option_object().get_cluster_replication_server_port();
			int cl_repl_concurrency = ini_option_object().get_cluster_replication_concurrency();

			if (this->_cluster_replication->is_started()
					&& (cl_repl_server_name != this->_cluster_replication->get_server_name()
							|| cl_repl_server_port != this->_cluster_replication->get_server_port())) {
				this->_cluster_replication->stop();
			}

			this->_cluster_replication->start(cl_repl_server_name, cl_repl_server_port, cl_repl_concurrency,
				   this->_storage, this->_cluster);
		} else {
			this->_cluster_replication->stop();
		}
	}

	log_notice("process successfully reloaded", 0);

	return 0;
}

/**
 *	novadbd application shutdown procs
 */
int novadbd::shutdown() {
	log_notice("shutting down active, and pool threads...", 0);
	this->_req_thread_pool->shutdown();
	this->_other_thread_pool->shutdown();
	log_notice("all threads are successfully shutdown", 0);

	log_notice("closing storage...", 0);
	this->_storage->close();

	this->_clear_pid();

	return 0;
}

/**
 *	notified storage error event procs
 */
void novadbd::on_storage_error() {
	status_node* s = dynamic_cast<status_node*>(status_object);
	if (!s) {
		log_err("status_object dynamic cast failed", 0);
		return;
	}
	s->set_node_status_code(status_node::node_status_storage_error);
}
// }}}

// {{{ protected methods
string novadbd::_get_pid_path() {
	return ini_option_object().get_pid_path().empty() ?
					ini_option_object().get_data_dir() + "/" + this->_ident + ".pid" :
					ini_option_object().get_pid_path();
};
// }}}

// {{{ private methods
/**
 *	set resource limit
 */
int novadbd::_set_resource_limit() {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
		log_err("getrlimit() failed: %s (%d)", util::strerror(errno), errno);
		return -1;
	} else {
		rlim_t fd_limit = ini_option_object().get_max_connection() * 2 + 16;
		if (rl.rlim_cur < fd_limit) {
			rl.rlim_cur = fd_limit;
			if (rl.rlim_max < rl.rlim_cur) {
				rl.rlim_max = rl.rlim_cur;
			}
			if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
				log_err("setrlimit() failed: %s (%d) - please run as root", util::strerror(errno), errno);
				return -1;
			}
		}
		log_info("setting resource limit (RLIMIT_NOFILE): %d", fd_limit);
	}

	return 0;
}

/**
 *	setup signal handler(s)
 */
int novadbd::_set_signal_handler() {
	struct sigaction sa;

	// SIGTERM/SIGINT
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_term_handler;
	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGTERM, util::strerror(errno), errno);
		return -1;
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_term_handler;
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGINT, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sigterm/sigint handler", 0);

	// SIGHUP
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_hup_handler;
	if (sigaction(SIGHUP, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGHUP, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sighup handler", 0);

	// SIGUSR1
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sa_usr1_handler;
	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		log_err("sigaction for %d failed: %s (%d)", SIGUSR1, util::strerror(errno), errno);
		return -1;
	}
	log_info("set up sigusr1 handler", 0);

	// signal mask
	sigset_t ss;
	sigfillset(&ss);
	sigdelset(&ss, SIGTERM);
	sigdelset(&ss, SIGINT);
	sigdelset(&ss, SIGHUP);
#if __APPLE__
	sigdelset(&ss, SIGPIPE);
#endif
	if (sigprocmask(SIG_SETMASK, &ss, NULL) < 0) {
		log_err("sigprocmask() failed: %s (%d)", util::strerror(errno), errno);
	}

	return 0;
}
// }}}

}	// namespace flare
}	// namespace gree

// {{{ ::main (entry point)
int main(int argc, char **argv) {
	gree::flare::novadbd& f = gree::flare::singleton<gree::flare::novadbd>::instance();
	f.set_ident("novadbd");

	if (f.startup(argc, argv) < 0) {
		return -1;
	}
	int r = f.run();
	f.shutdown();

	return r;
}
// }}}

// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent

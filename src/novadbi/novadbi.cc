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
 *	novadbi.cc
 *
 *	implementation of gree::flare::novadbi (and some other global stuffs)
 *
 *	@author	Masaki Fujimoto <fujimoto@php.net>
 *
 *	$Id$
 */
#include "novadbi.h"
#include "coordinator_factory.h"
#include "connection_tcp.h"

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

	singleton<novadbi>::instance().request_shutdown();

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
 *	ctor for novadbi
 */
novadbi::novadbi():
		_server(NULL),
		_thread_index(NULL),
		_req_thread_pool(NULL),
		_other_thread_pool(NULL),
		_cluster(NULL),
		_coordinator(NULL) {
}

/**
 *	dtor for novadbi
 */
novadbi::~novadbi() {
	delete this->_server;
	this->_server = NULL;

	delete this->_req_thread_pool;
	this->_other_thread_pool = NULL;

	delete this->_other_thread_pool;
	this->_other_thread_pool = NULL;

	delete this->_thread_index;
	this->_thread_index = NULL;

	delete this->_cluster;
	this->_cluster = NULL;

	if (this->_coordinator != NULL) {
		this->_coordinator_factory.destroy_coordinator(this->_coordinator);
		this->_coordinator = NULL;
	}

	delete stats_object;
	stats_object = NULL;

	delete status_object;
	status_object = NULL;
}
// }}}

// {{{ operator overloads
// }}}

// {{{ public methods
/**
 *	novadbi application startup procs
 */
int novadbi::startup(int argc, char **argv) {
	ini_option_object().set_args(argc, argv);
	if (ini_option_object().load() < 0) {
		return -1;
	}

	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility(), ini_option_object().get_log_stderr());
	stats_object = new stats_index();
	stats_object->startup();

	status_object = new status_index();

	log_notice("%s version %s - system logger started", this->_ident.c_str(), PACKAGE_VERSION);

	log_notice("application startup in progress...", 0);
	log_notice("  config_path:            %s", ini_option_object().get_config_path().c_str());
	log_notice("  pid_path:               %s", ini_option_object().get_pid_path().c_str());
	log_notice("  stderr:                 %s", ini_option_object().get_log_stderr() ? "true" : "false");
	log_notice("  daemonize:              %s", ini_option_object().is_daemonize() ? "true" : "false");
	log_notice("  data_dir:               %s", ini_option_object().get_data_dir().c_str());
	log_notice("  max_connection:         %d", ini_option_object().get_max_connection());
	log_notice("  monitor_threshold:      %d", ini_option_object().get_monitor_threshold());
	log_notice("  monitor_interval:       %d", ini_option_object().get_monitor_interval());
	log_notice("  monitor_read_timeout:   %d", ini_option_object().get_monitor_read_timeout());
	log_notice("  net_read_timeout:       %d", ini_option_object().get_net_read_timeout());
	log_notice("  server_name:            %s", ini_option_object().get_server_name().c_str());
	log_notice("  server_port:            %d", ini_option_object().get_server_port());
	log_notice("  server_socket:          %s", ini_option_object().get_server_socket().c_str());
	log_notice("  stack_size:             %d", ini_option_object().get_stack_size());
	log_notice("  thread_pool_size:       %d", ini_option_object().get_thread_pool_size());

	// startup procs
	if (this->_set_resource_limit() < 0) {
		return -1;
	}

	if (ini_option_object().is_daemonize()) {
		if (this->_daemonize() < 0) {
			return -1;
		}
	}

	string index_db               = ini_option_object().get_index_db();
	int partition_modular_hint    = ini_option_object().get_partition_modular_hint();
	int partition_modular_virtual = ini_option_object().get_partition_modular_virtual();
	int partition_size            = ini_option_object().get_partition_size();
	string partition_type         = ini_option_object().get_partition_type();
	string key_hash_algorithm     = ini_option_object().get_key_hash_algorithm();	

	// create a coordinator from option [index-db]
	if (index_db.empty()) {
		index_db = "file://"+ini_option_object().get_data_dir();
	}
	ostringstream myname_oss;
	myname_oss << ini_option_object().get_server_name() << ":" << ini_option_object().get_server_port();
	this->_coordinator = this->_coordinator_factory.create_coordinator(index_db, myname_oss.str());
	if (this->_coordinator == NULL) {
		log_warning("failed to create a coordinator: %s", index_db.c_str());
		return -1;
	}

	// startup meta info with a coordinator
	map<string,string> meta_variables;
	if (this->_coordinator->get_meta_variables(meta_variables) < 0) {
		log_warning("failed to get meta variables", 0);
		return -1;
	}
	if (meta_variables.count("partition-modular-hint") > 0) {
		try {
			partition_modular_hint = boost::lexical_cast<int>(meta_variables["partition-modular-hint"]);
		} catch (boost::bad_lexical_cast& e) {
			log_warning("invalid partition-modular-hint: %s", meta_variables["partition-modular-hint"].c_str());
			return -1;
		}
	}
	if (meta_variables.count("partition-modular-virtual") > 0) {
		try {
			partition_modular_virtual = boost::lexical_cast<int>(meta_variables["partition-modular-virtual"]);
		} catch (boost::bad_lexical_cast& e) {
			log_warning("invalid partition-modular-virtual: %s", meta_variables["partition-modular-virtual"].c_str());
			return -1;
		}
	}
	if (meta_variables.count("partition-size") > 0) {
		try {
			partition_size = boost::lexical_cast<int>(meta_variables["partition-size"]);
		} catch (boost::bad_lexical_cast& e) {
			log_warning("invalid partition-size: %s", meta_variables["partition-size"].c_str());
			return -1;
		}
	}
	if (meta_variables.count("partition-type") > 0) {
		partition_type = meta_variables["partition-type"];
	}
	if (meta_variables.count("key-hash-algorithm") > 0) {
		key_hash_algorithm = meta_variables["key-hash-algorithm"];
	}

	log_notice("  index_db:               %s", index_db.c_str());
	log_notice("  partition_modular_hint: %d", partition_modular_hint);
	log_notice("  partition_modular_virtual: %d", partition_modular_virtual);
	log_notice("  partition_size:         %d", partition_size);
	log_notice("  partition_type:         %s", partition_type.c_str());
	log_notice("  key_hash_algorithm:     %s", key_hash_algorithm.c_str());

	if (this->_set_signal_handler() < 0) {
		return -1;
	}

	// application objects
	connection_tcp::read_timeout = ini_option_object().get_net_read_timeout() * 1000;		// -> msec
	this->_server = new server();
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

	this->_cluster = new cluster(this->_req_thread_pool, this->_other_thread_pool, ini_option_object().get_server_name(), ini_option_object().get_server_port());
	this->_cluster->set_monitor_threshold(ini_option_object().get_monitor_threshold());
	this->_cluster->set_monitor_interval(ini_option_object().get_monitor_interval());
	this->_cluster->set_monitor_read_timeout(ini_option_object().get_monitor_read_timeout());

	this->_cluster->set_partition_size(partition_size);
	storage::hash_algorithm ha = storage::hash_algorithm_simple;
	if (storage::hash_algorithm_cast(key_hash_algorithm, ha) < 0
			|| ha == storage::hash_algorithm_bitshift // used internally up to 1.0.15
			|| ha == storage::hash_algorithm_adler32  // used internally after 1.0.16
			|| ha == storage::hash_algorithm_murmur  // used internally after 1.0.18
			|| ha == this->_cluster->get_proxy_hash_algorithm()) {
		return -1;
	}
	this->_cluster->set_key_hash_algorithm(ha);

	shared_thread cth = this->_other_thread_pool->get(thread_pool::thread_type_controller);
	handler_controller* ch = new handler_controller(cth, this->_cluster);
	cth->trigger(ch);

	key_resolver::type t;
	key_resolver::type_cast(partition_type, t);
	// XXX: fix this interface...just passing key resolver object will do? refactor when another partition type is added
	if (this->_cluster->startup_index(this->_coordinator, t, partition_modular_hint, partition_modular_virtual) < 0) {
		return -1;
	}

	shared_thread ath = this->_other_thread_pool->get(thread_pool::thread_type_alarm);
	handler_alarm* ah = new handler_alarm(ath);
	ath->trigger(ah);

	if (this->_set_pid() < 0) {
		return -1;
	}

	return 0;
}

/**
 *	novadbi application running loop
 */
int novadbi::run() {
	log_notice("entering running loop", 0);

	for (;;) {
		if (this->_shutdown_request) {
			log_notice("shutdown request accepted -> breaking running loop", 0);
			break;
		}

		if (reload_request) {
			log_notice("received signal [SIGHUP] -> reloading", 0);
			singleton<novadbi>::instance().reload();

			// Reset signal flag
			reload_request = 0;
		}

		vector<shared_connection_tcp> connection_list = this->_server->wait();

		if (reload_request) {
			log_notice("received signal [SIGHUP] -> reloading", 0);
			singleton<novadbi>::instance().reload();

			// Reset signal flag
			reload_request = 0;
		}

		vector<shared_connection_tcp>::iterator it;
		for (it = connection_list.begin(); it != connection_list.end(); it++) {
			shared_connection_tcp c = *it;

			if (this->_req_thread_pool->get_thread_size(thread_pool::thread_type_request) >= ini_option_object().get_max_connection()) {
				log_warning("too many connection [%d] -> closing socket and continue", ini_option_object().get_max_connection());
				continue;
			}

			stats_object->increment_total_connections();

			shared_thread t = this->_req_thread_pool->get(thread_pool::thread_type_request);
			handler_request* h = new handler_request(t, c);
			t->trigger(h);
		}
	}

	return 0;
}

/**
 *	novadbi application reload procs
 */
int novadbi::reload() {
	if (ini_option_object().reload() < 0) {
		log_notice("invalid config file -> skip reloading", 0);
		return -1;
	}

	// log_facility
	log_notice("re-opening syslog...", 0);
	singleton<logger>::instance().close();
	singleton<logger>::instance().open(this->_ident, ini_option_object().get_log_facility(), ini_option_object().get_log_stderr());

	this->_cluster->set_monitor_threshold(ini_option_object().get_monitor_threshold());
	this->_cluster->set_monitor_interval(ini_option_object().get_monitor_interval());
	this->_cluster->set_monitor_read_timeout(ini_option_object().get_monitor_read_timeout());

	// net_read_timeout
	connection_tcp::read_timeout = ini_option_object().get_net_read_timeout() * 1000;		// -> msec

	// thread_pool_size
	this->_req_thread_pool->set_max_pool_size(ini_option_object().get_thread_pool_size());

	// re-setup resource limit (do not care about return value here)
	this->_set_resource_limit();

	log_notice("process successfully reloaded", 0);

	return 0;
}

/**
 *	novadbi application shutdown procs
 */
int novadbi::shutdown() {
	log_notice("shutting down active, and pool threads...", 0);
	this->_req_thread_pool->shutdown();
	this->_other_thread_pool->shutdown();
	log_notice("all threads are successfully shutdown", 0);

	this->_clear_pid();

	return 0;
}
// }}}

// {{{ protected methods
string novadbi::_get_pid_path() {
	return ini_option_object().get_pid_path().empty() ?
					ini_option_object().get_data_dir() + "/" + this->_ident + ".pid" :
					ini_option_object().get_pid_path();
};
// }}}

// {{{ private methods
/**
 *	set resource limit
 */
int novadbi::_set_resource_limit() {
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
int novadbi::_set_signal_handler() {
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
	gree::flare::novadbi& f = gree::flare::singleton<gree::flare::novadbi>::instance();
	f.set_ident("novadbi");

	if (f.startup(argc, argv) < 0) {
		return -1;
	}
	int r = f.run();
	f.shutdown();

	return r;
}
// }}}

// vim: foldmethod=marker tabstop=2 shiftwidth=2 autoindent

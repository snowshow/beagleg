/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2013, 2014 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/types.h>
#include <unistd.h>

#include "gcode-machine-control.h"
#include "gcode-parser.h"
#include "motor-interface.h"

// Some default settings. These are most likely overrridden via flags by user.

// All these settings are in sequence of enum GCodeParserAxes: XYZEABC
static const float kMaxFeedrate[GCODE_NUM_AXES] =
  {  200,  200,  90,     10, 1, 0, 0 };

static const float kDefaultAccel[GCODE_NUM_AXES]=
  { 4000, 4000, 1000, 10000, 1, 0, 0 };

static const float kStepsPerMM[GCODE_NUM_AXES]  =
  {  160,  160,  160,    40, 1, 0, 0 };

static const enum HomeType kHomePos[GCODE_NUM_AXES] =
  { HOME_POS_ORIGIN, HOME_POS_ORIGIN, HOME_POS_ORIGIN,
    HOME_POS_NONE, HOME_POS_NONE, HOME_POS_NONE, HOME_POS_NONE };

static const float kMoveRange[GCODE_NUM_AXES] =
  { 100, 100, 100, -1, -1, -1, -1 };

// This is the channel layout on the Bumps-board ( github.com/hzeller/bumps ),
// currently the only cape existing for BeagleG, so we can as well hardcode it.
static const char kChannelLayout[] = "23140";

// Output mapping from left to right.
static const char kAxisMapping[] = "XYZEA";

static int usage(const char *prog, const char *msg) {
  if (msg) {
    fprintf(stderr, "%s\n\n", msg);
  }
  fprintf(stderr, "Usage: %s [options] [<gcode-filename>]\n"
	  "Options:\n"
	  "  --steps-mm <axis-steps>   : steps/mm, comma separated "
	  "(Default 160,160,160,40,0, ...).\n"
	  "  --max-feedrate <rate> (-m): Max. feedrate per axis (mm/s), "
	  "comma separated (Default: 200,200,90,10,0, ...).\n"
	  "  --accel <accel>       (-a): Acceleration per axis (mm/s^2), "
	  "comma separated (Default 4000,4000,1000,10000,0, ...).\n"
#if 0   // not yet implemented
	  "  --home-pos <0/1/2>,*      : Home positions of axes, comma "
	  "separated\n"
	  "                                0 = none, 1 = origin; "
	  "2 = end-of-range (Default: 1,1,1,0,...).\n"
	  "  --range <range-mm>    (-r): Comma separated range of of axes in mm (0..range[axis])"
	  ". Only\n"
	  "                               values > 0 are actively clipped. "
	  "(Default: 100,100,100,-1,-1, ...)\n"
#endif
	  "  --axis-mapping            : Axis letter mapped to which motor "
          "connector (=string pos)\n"
	  "                              Use letter or '_' for empty slot. "
	  "(Default: 'XYZEABC')\n"
	  "  --port <port>         (-p): Listen on this TCP port.\n"
	  "  --bind-addr <bind-ip> (-b): Bind to this IP (Default: 0.0.0.0).\n"
	  "  -f <factor>               : Print speed factor (Default 1.0).\n"
	  "  -n                        : Dryrun; don't send to motors "
	  "(Default: off).\n"
	  "  -P                        : Verbose: Print motor commands "
	  "(Default: off).\n"
	  "  -S                        : Synchronous: don't queue "
	  "(Default: off).\n"
	  "  -R                        : Repeat file forever.\n",
	  prog);
  fprintf(stderr, "All comma separated axis numerical values are in the "
	  "sequence X,Y,Z,E,A,B,C,U,V,W\n");
  fprintf(stderr, "You can either specify --port <port> to listen for commands "
	  "or give a filename\n");
  return 1;
}

// Reads the given "gcode_filename" with GCode and operates machine with it.
// If "do_loop" is 1, repeats this forever (e.g. for stress test).
static int send_file_to_machine(const char *gcode_filename, char do_loop) {
  do {
    int fd = open(gcode_filename, O_RDONLY);
    if (gcode_machine_control_from_stream(fd, STDERR_FILENO) != 0)
      return 1;
  } while (do_loop);
  return 0;
}

// Run TCP server on "bind_addr" (can be NULL, then it is 0.0.0.0) and "port".
// Interprets GCode coming from a connection. Only one connection at a
// time can be active.
static int run_server(const char *bind_addr, int port) {
  if (port > 65535) {
    fprintf(stderr, "Invalid port %d\n", port);
    return 1;
  }
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("creating socket");
    return 1;
  }

  struct sockaddr_in serv_addr;
  bzero(&serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  if (bind_addr && !inet_pton(AF_INET, bind_addr, &serv_addr.sin_addr.s_addr)) {
    fprintf(stderr, "Invalid bind IP address %s\n", bind_addr);
    return 1;
  }
  serv_addr.sin_port = htons(port);
  int on = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(s, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    perror("trouble binding");
    return 1;
  }

  signal(SIGPIPE, SIG_IGN);  // Pesky clients, closing connections...

  listen(s, 2);
  printf("Listening on %s:%d\n", bind_addr ? bind_addr : "0.0.0.0", port);

  int process_result;
  do {
    struct sockaddr_in client;
    socklen_t socklen = sizeof(client);
    int connection = accept(s, (struct sockaddr*) &client, &socklen);
    if (connection < 0) {
      perror("accept");
      return 1;
    }
    char ip_buffer[INET_ADDRSTRLEN];
    const char *print_ip = inet_ntop(AF_INET, &client.sin_addr,
				     ip_buffer, sizeof(ip_buffer));
    printf("Accepting new connection from %s\n", print_ip);
    process_result = gcode_machine_control_from_stream(connection, connection);
    printf("Connection to %s closed.\n", print_ip);
  } while (process_result == 0);

  close(s);
  fprintf(stderr, "Last gcode_machine_control_from_stream() == %d. Exiting\n",
	  process_result);
  return 0;
}

// Parse comma (or other character) separated array of up to "count" float
// numbers and fill into result[]. Returns number of elements parsed on success.
static int parse_float_array(const char *input, float result[], int count) {
  for (int i = 0; i < count; ++i) {
    char *end;
    result[i] = strtod(input, &end);
    if (end == input) return 0;  // parse error.
    if (*end == '\0') return i + 1;
    input = end + 1;
  }
  return count;
}

int main(int argc, char *argv[]) {
  struct MachineControlConfig config;
  bzero(&config, sizeof(config));
  memcpy(config.steps_per_mm, kStepsPerMM, sizeof(config.steps_per_mm));
  memcpy(config.home_switch, kHomePos, sizeof(config.home_switch));
  memcpy(config.move_range_mm, kMoveRange, sizeof(config.move_range_mm));
  memcpy(config.max_feedrate, kMaxFeedrate, sizeof(config.max_feedrate));
  memcpy(config.acceleration, kDefaultAccel, sizeof(config.acceleration));
  config.speed_factor = 1;
  config.dry_run = 0;
  config.debug_print = 0;
  config.synchronous = 0;
  config.channel_layout = kChannelLayout;
  config.axis_mapping = kAxisMapping;

  // Less common options don't have a short option.
  enum LongOptionsOnly {
    SET_STEPS_MM = 1000,
    SET_HOME_POS,
    SET_MOTOR_MAPPING,
  };

  static struct option long_options[] = {
    { "max-feedrate",  required_argument, NULL, 'm'},
    { "accel",         required_argument, NULL, 'a'},
    { "range",         required_argument, NULL, 'r' },
    { "steps-mm",      required_argument, NULL, SET_STEPS_MM },
    { "home-pos",      required_argument, NULL, SET_HOME_POS },
    { "axis-mapping",  required_argument, NULL, SET_MOTOR_MAPPING },
    { "port",          required_argument, NULL, 'p'},
    { "bind-addr",     required_argument, NULL, 'b'},
    { 0,               0,                 0,    0  },
  };

  int listen_port = -1;
  char do_file_repeat = 0;
  char *bind_addr = NULL;
  int opt;
  int parse_count;
  while ((opt = getopt_long(argc, argv, "m:a:p:b:r:SPRnf:",
			    long_options, NULL)) != -1) {
    switch (opt) {
    case 'f':
      config.speed_factor = atof(optarg);
      if (config.speed_factor <= 0)
	return usage(argv[0], "Speedfactor cannot be <= 0");
      break;
    case 'm':
      parse_count = parse_float_array(optarg, config.max_feedrate,
                                      GCODE_NUM_AXES);
      if (!parse_count) return usage(argv[0], "max-feedrate missing.");
      break;
    case 'a':
      parse_count = parse_float_array(optarg, config.acceleration,
                                      GCODE_NUM_AXES);
      if (!parse_count) return usage(argv[0], "Acceleration missing.");
      // Negative or 0 means: 'infinite'.
      break;
    case SET_STEPS_MM:
      if (!parse_float_array(optarg, config.steps_per_mm, GCODE_NUM_AXES))
	return usage(argv[0], "steps/mm failed to parse.");
      break;
    case SET_MOTOR_MAPPING:
      config.axis_mapping = strdup(optarg);
      break;
    case SET_HOME_POS: {
      float tmp[GCODE_NUM_AXES];
      bzero(tmp, sizeof(tmp));
      if (!parse_float_array(optarg, tmp, GCODE_NUM_AXES))
	return usage(argv[0], "Failed to parse home switch.");
      for (int i = 0; i < GCODE_NUM_AXES; ++i)
	config.home_switch[i] = (enum HomeType) tmp[i];
    }
      break;
    case 'r':
      if (!parse_float_array(optarg, config.move_range_mm, GCODE_NUM_AXES))
	return usage(argv[0], "Failed to parse ranges.");
      break;
    case 'n':
      config.dry_run = 1;
      break;
    case 'P':
      config.debug_print = 1;
      break;
    case 'S':
      config.synchronous = 1;
      break;
    case 'R':
      do_file_repeat = 1;
      break;
    case 'p':
      listen_port = atoi(optarg);
      break;
    case 'b':
      bind_addr = strdup(optarg);
      break;
    default:
      return usage(argv[0], "Unknown flag");
    }
  }

  const char has_filename = (optind < argc);
  if (! (has_filename ^ (listen_port > 0))) {
    return usage(argv[0], "Choose one: <gcode-filename> or --port <port>.");
  }
  if (!has_filename && do_file_repeat) {
    return usage(argv[0], "-R (repeat) only makes sense with a filename.");
  }

  if (gcode_machine_control_init(&config) != 0) {
    return 1;
  }

  int ret = 0;
  if (has_filename) {
    const char *filename = argv[optind];
    ret = send_file_to_machine(filename, do_file_repeat);
  } else {
    ret = run_server(bind_addr, listen_port);
  }

  gcode_machine_control_exit();
  free(bind_addr);
  return ret;
}

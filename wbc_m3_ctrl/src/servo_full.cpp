/*
 * Whole-Body Control for Human-Centered Robotics http://www.me.utexas.edu/~hcrl/
 *
 * Copyright (c) 2011 University of Texas at Austin. All rights reserved.
 *
 * Author: Roland Philippsen
 *
 * BSD license:
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of
 *    contributors to this software may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR THE CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <wbc_m3_ctrl/rt_util_full.h>

// one of these just for logging timestamp
#include <rtai_sched.h>
#include <rtai_shm.h>
#include <rtai.h>
#include <rtai_sem.h>
#include <rtai_nam2num.h>
#include <rtai_registry.h>

#include <ros/ros.h>
#include <jspace/test/sai_util.hpp>
#include <opspace/Skill.hpp>
#include <opspace/Factory.hpp>
#include <uta_opspace/ControllerNG.hpp>
#include <uta_opspace/HelloGoodbyeSkill.hpp>
#include <uta_opspace/TaskPostureSkill.hpp>
#include <uta_opspace/WriteSkill.hpp>
#include <uta_opspace/StaticAccuracyTest.hpp>
#include <uta_opspace/DynamicAccuracyTest.hpp>
#include <uta_opspace/TrajAccuracyTest.hpp>
#include <uta_opspace/CircleTest.hpp>
#include <uta_opspace/GestureSkill.hpp>
#include <uta_opspace/JointTest.hpp>
#include <uta_opspace/MultiJointPos.hpp>
#include <uta_opspace/Reach.hpp>
#include <uta_opspace/CoffeeGrindSkill.hpp>
#include <wbc_core/opspace_param_callbacks.hpp>
#include <boost/scoped_ptr.hpp>
#include <err.h>
#include <signal.h>
#include <wbc_m3_ctrl/handcontroller.h>
#include <wbc_m3_ctrl/headcontroller.h>
#include <wbc_m3_ctrl/headlookcontroller.h>
#include <wbc_m3_ctrl/torsocontroller.h>

using namespace wbc_m3_ctrl;
using namespace opspace;
using namespace wbc_core_opspace;
using namespace uta_opspace;
using namespace boost;
using namespace std;

static char const * opspace_fallback_str = 
  "- tasks:\n"
  "  - type: opspace::PositionTask\n"
  "    name: eepos\n"
  "    end_effector_id: 6\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  10.0 ]\n"
  "    maxvel: [ 0.5 ]\n"
  "    maxacc: [ 1.5 ]\n"
  "  - type: opspace::PostureTask\n"
  "    name: posture\n"
  "    dt_seconds: 0.002\n"
  "    kp: [ 100.0 ]\n"
  "    kd: [  10.0 ]\n"
  "    maxvel: [ 3.1416 ]\n"
  "    maxacc: [ 6.2832 ]\n"
  "- skills:\n"
  "  - type: opspace::TPSkill\n"
  "    name: task_posture\n"
  "    default:\n"
  "      eepos: eepos\n"
  "      posture: posture\n";


static bool verbose(false);
static scoped_ptr<jspace::Model> model;
static shared_ptr<Factory> factory;
static shared_ptr<opspace::ReflectionRegistry> registry;
static long long servo_rate;
static long long actual_servo_rate;
static shared_ptr<ParamCallbacks> param_cbs;
static shared_ptr<ControllerNG> controller;
static HeadController* head_controller;
static TorsoController* torso_controller;
static HandController* hand_controller;


static void usage(int ecode, std::string msg)
{
  errx(ecode,
       "%s\n"
       "  options:\n"
       "  -h               help (this message)\n"
       "  -v               verbose mode\n"
       "  -r  <filename>   robot specification (SAI XML format)\n"
       "  -f  <frequency>  servo rate (integer number in Hz, default 500Hz)\n"
       "  -s  <filename>   skill specification (YAML file with tasks etc)",
       msg.c_str());
}


static void parse_options(int argc, char ** argv)
{
  string skill_spec("");
  string robot_spec("");
  servo_rate = 500;
  
  for (int ii(1); ii < argc; ++ii) {
    if ((strlen(argv[ii]) < 2) || ('-' != argv[ii][0])) {
      usage(EXIT_FAILURE, "problem with option `" + string(argv[ii]) + "'");
    }
    else
      switch (argv[ii][1]) {
	
      case 'h':
	usage(EXIT_SUCCESS, "servo [-h] [-v] [-s skillspec] -r robotspec");
	
      case 'v':
	verbose = true;
 	break;
	
      case 'r':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-r requires parameter");
 	}
	robot_spec = argv[ii];
 	break;
	
      case 'f':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-f requires parameter");
 	}
	else {
	  istringstream is(argv[ii]);
	  is >> servo_rate;
	  if ( ! is) {
	    usage(EXIT_FAILURE, "failed to read servo rate from `" + string(argv[ii]) + "'");
	  }
	  if (0 >= servo_rate) {
	    usage(EXIT_FAILURE, "servo rate has to be positive");
	  }
	}
 	break;
	
      case 's':
 	++ii;
 	if (ii >= argc) {
	  usage(EXIT_FAILURE, "-s requires parameter");
 	}
	skill_spec = argv[ii];
 	break;
	
      default:
	usage(EXIT_FAILURE, "invalid option `" + string(argv[ii]) + "'");
      }
  }
  
  try {
    if (robot_spec.empty()) {
      usage(EXIT_FAILURE, "no robot specification (see option -r)");
    }
    if (verbose) {
      warnx("reading robot spec from %s", robot_spec.c_str());
    }
    static bool const enable_coriolis_centrifugal(false);
    model.reset(jspace::test::parse_sai_xml_file(robot_spec, enable_coriolis_centrifugal));
  }
  catch (runtime_error const & ee) {
    errx(EXIT_FAILURE,
	 "exception while parsing robot specification\n"
	 "  filename: %s\n"
	 "  error: %s",
	 robot_spec.c_str(), ee.what());
  }
  
  factory.reset(new Factory());
  
  Status st;
  if (skill_spec.empty()) {
    if (verbose) {
      warnx("using fallback task/posture skill");
    }
    st = factory->parseString(opspace_fallback_str);
  }
  else {
    if (verbose) {
      warnx("reading skills from %s", skill_spec.c_str());
    }
    st = factory->parseFile(skill_spec);
  }
  if ( ! st) {
    errx(EXIT_FAILURE,
	 "failed to parse skills\n"
	 "  specification file: %s\n"
	 "  error description: %s",
	 skill_spec.c_str(), st.errstr.c_str());
  }
  if (verbose) {
    factory->dump(cout, "*** parsed tasks and skills", "* ");
  }
}


static void handle(int signum)
{
  if (ros::ok()) {
    warnx("caught signal, requesting shutdown");
    ros::shutdown();
  }
  else {
    errx(EXIT_SUCCESS, "caught signal (again?), attempting forced exit");
  }
}


namespace {
  
  
  class Servo
    : public RTUtilFull
  {
  public:
    shared_ptr<Skill> skill;
    
    virtual int init(jspace::State const & head_state,
		     jspace::State const & arm_state,
		     jspace::State const & hand_state,
		     jspace::State const & torso_state) {
      if (skill) {
	warnx("Servo::init(): already initialized");
	return -1;
      }
      if (factory->getSkillTable().empty()) {
	warnx("Servo::init(): empty skill table");
	return -2;
      }
      if ( ! model) {
	warnx("Servo::init(): no model");
	return -3;
      }
      
      model->update(arm_state);
    
      jspace::Status status(controller->init(*model));
      if ( ! status) {
	warnx("Servo::init(): controller->init() failed: %s", status.errstr.c_str());
	return -4;
      }
      
      skill = factory->getSkillTable()[0]; // XXXX to do: allow selection at runtime
      status = skill->init(*model);
      if ( ! status) {
	warnx("Servo::init(): skill->init() failed: %s", status.errstr.c_str());	skill.reset();
	return -5;
      }

      head_controller = new HeadController();
      hand_controller = new HandController();
      torso_controller = new TorsoController();

      status = head_controller->init(head_state);
      if ( ! status) {
	warnx("Servo::init(): head_controller->init() failed: %s", status.errstr.c_str());
	return -4;
      }
      status = torso_controller->init(torso_state);
      if ( ! status) {
	warnx("Servo::init(): torso_controller->init() failed: %s", status.errstr.c_str());
	return -4;
      }
      status = hand_controller->init(hand_state);
      if ( ! status) {
	warnx("Servo::init(): hand_controller->init() failed: %s", status.errstr.c_str());
	return -4;
      }
      return 0;
    }
    
    
    virtual int update(jspace::State const & head_state,
		       jspace::Vector & head_command,
		       jspace::State const & arm_state,
		       jspace::Vector & arm_command,
		       jspace::State const & hand_state,
		       jspace::Vector & hand_command,
		       jspace::State const & torso_state,
		       jspace::Vector & torso_command)
    {
      if ( ! skill) {
	warnx("Servo::update(): not initialized\n");
	return -1;
      }
      
      model->update(arm_state);
      
      jspace::Status status(controller->computeCommand(*model, *skill, arm_command));
      if ( ! status) {
	warnx("Servo::update(): controller->computeCommand() failed: %s", status.errstr.c_str());
	return -2;
      }

      jspace::Transform gl_trans;
      if ( ! model->computeGlobalFrame(model->getNode(6),jspace::Vector::Zero(3),gl_trans)) {
	warnx("Servo::Update(): model->computeGlobalFrame() failed");
	return -2;
      }
      
      Vector eepos(gl_trans.translation());
      head_controller->update(head_state);
      status = head_controller->computeCommand(head_command);
      if ( ! status) {
	warnx("Servo::update(): head_controller->computeCommand() failed: %s", status.errstr.c_str());
	return -2;
      }

      torso_controller->update(torso_state);
      status = torso_controller->computeCommand(torso_command);
      if ( ! status) {
	warnx("Servo::update(): torso_controller->computeCommand() failed: %s", status.errstr.c_str());
	return -2;
      }

      hand_controller->update(hand_state);
      status = hand_controller->computeCommand(hand_command);
      if ( ! status) {
	warnx("Servo::update(): hand_controller->computeCommand() failed: %s", status.errstr.c_str());
	return -2;
      }
      
      return 0;
    }

    virtual int cleanup(void)
    {
      skill.reset();
      return 0;
    }
    
    
    virtual int slowdown(long long iteration,
			 long long desired_ns,
			 long long actual_ns)
    {
      actual_servo_rate = 1000000000 / actual_ns;
      return 0;
    }
  };
  
}


int main(int argc, char ** argv)
{
  struct sigaction sa;
  bzero(&sa, sizeof(sa));
  sa.sa_handler = handle;
  if (0 != sigaction(SIGINT, &sa, 0)) {
    err(EXIT_FAILURE, "sigaction");
  }
  
  // Before we attempt to read any tasks and skills from the YAML
  // file, we need to inform the static type registry about custom
  // additions such as the HelloGoodbyeSkill.
  Factory::addSkillType<uta_opspace::HelloGoodbyeSkill>("uta_opspace::HelloGoodbyeSkill");
  Factory::addSkillType<uta_opspace::TaskPostureSkill>("uta_opspace::TaskPostureSkill");
  Factory::addSkillType<uta_opspace::WriteSkill>("uta_opspace::WriteSkill");
  Factory::addSkillType<uta_opspace::StaticAccuracyTest>("uta_opspace::StaticAccuracyTest");
  Factory::addSkillType<uta_opspace::DynamicAccuracyTest>("uta_opspace::DynamicAccuracyTest");
  Factory::addSkillType<uta_opspace::TrajAccuracyTest>("uta_opspace::TrajAccuracyTest");
  Factory::addSkillType<uta_opspace::CircleTest>("uta_opspace::CircleTest");
  Factory::addSkillType<uta_opspace::GestureSkill>("uta_opspace::GestureSkill");
  Factory::addSkillType<uta_opspace::JointTest>("uta_opspace::JointTest");
  Factory::addSkillType<uta_opspace::MultiJointPos>("uta_opspace::MultiJointPos");
  Factory::addSkillType<uta_opspace::Reach>("uta_opspace::Reach");
  Factory::addSkillType<uta_opspace::CoffeeGrindSkill>("uta_opspace::CoffeeGrindSkill");
  
  ros::init(argc, argv, "wbc_m3_ctrl_servo", ros::init_options::NoSigintHandler);
  parse_options(argc, argv);
  ros::NodeHandle node("~");
  
  controller.reset(new ControllerNG("wbc_m3_ctrl::servo"));
  param_cbs.reset(new ParamCallbacks());
  Servo servo;
  try {
    if (verbose) {
      warnx("initializing param callbacks");
    }
    registry.reset(factory->createRegistry());
    registry->add(controller);
    param_cbs->init(node, registry, 1, 100);
    
    if (verbose) {
      warnx("starting servo with %lld Hz", servo_rate);
    }
    actual_servo_rate = servo_rate;
    servo.start(servo_rate);
  }
  catch (std::runtime_error const & ee) {
    errx(EXIT_FAILURE, "failed to start servo: %s", ee.what());
  }
  
  warnx("started servo RT thread");
  ros::Time dbg_t0(ros::Time::now());
  ros::Time dump_t0(ros::Time::now());
  ros::Duration dbg_dt(0.1);
  ros::Duration dump_dt(0.05);
  
  while (ros::ok()) {
    ros::Time t1(ros::Time::now());
    if (verbose) {
      if (t1 - dbg_t0 > dbg_dt) {
	dbg_t0 = t1;
	servo.skill->dbg(cout, "\n\n**************************************************", "");
	controller->dbg(cout, "--------------------------------------------------", "");
	cout << "--------------------------------------------------\n";
	jspace::pretty_print(model->getState().position_, cout, "arm jpos", "  ");
	jspace::pretty_print(model->getState().velocity_, cout, "arm jvel", "  ");
	jspace::pretty_print(model->getState().force_, cout, "jforce", "  ");
	jspace::pretty_print(controller->getCommand(), cout, "arm gamma", "  ");
	jspace::pretty_print(head_controller->getState().position_, cout, "head jpos", "  ");
	jspace::pretty_print(head_controller->getState().velocity_, cout, "head jvel", "  ");
	jspace::pretty_print(head_controller->getCommand(), cout, "head command", "  ");
	jspace::pretty_print(torso_controller->getState().position_, cout, "torso jpos", "  ");
	jspace::pretty_print(torso_controller->getState().velocity_, cout, "torso jvel", "  ");
	jspace::pretty_print(torso_controller->getCommand(), cout, "torso gamma", "  ");
	jspace::pretty_print(hand_controller->getState().position_, cout, "hand jpos", "  ");
	jspace::pretty_print(hand_controller->getState().velocity_, cout, "hand jvel", "  ");
	jspace::pretty_print(hand_controller->getCommand(), cout, "hand gamma", "  ");
	Vector gravity;
	model->getGravity(gravity);
	jspace::pretty_print(gravity, cout, "gravity", "  ");
	cout << "servo rate: " << actual_servo_rate << "\n";
      }
    }
    if (t1 - dump_t0 > dump_dt) {
      dump_t0 = t1;
      controller->qhlog(*servo.skill, rt_get_cpu_time_ns() / 1000);
    }
    ros::spinOnce();
    usleep(10000);		// 100Hz-ish
  }
  
  warnx("shutting down");
  servo.shutdown();
}
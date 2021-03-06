/*
 * Whole-Body Control for Human-Centered Robotics http://www.me.utexas.edu/~hcrl/
 *
 * Copyright (c) 2011 University of Texas at Austin. All rights reserved.
 *
 * Author: Roland Philippsen
 * modified by Josh Petersen for the base
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

#include <wbc_m3_ctrl/rt_util_base.h>

#include <rtai_sched.h>
#include <rtai_shm.h>
#include <rtai.h>
#include <rtai_sem.h>
#include <rtai_nam2num.h>
#include <rtai_registry.h>
#include <wbc_m3_ctrl/torque_feedback.h>

//#include "m3/shared_mem/torque_shm_sds.h"
#include "m3uta/controllers/torque_shm_uta_sds.h"
#include "m3/robots/chain_name.h"
#include <m3rt/base/m3ec_def.h>
#include <m3rt/base/m3rt_def.h>


#define TORQUE_SHM "TSHMM"
#define TORQUE_CMD_SEM "TSHMC"
#define TORQUE_STATUS_SEM "TSHMS"


namespace wbc_m3_ctrl {
  
  static rt_thread_state_t rt_thread_state(RT_THREAD_UNDEF);
  static int shutdown_request(0);
  static RT_TASK * nonrt_task(0);
  static int rt_thread_id(0);
  static long long rt_period_ns(-1); 
  
  static void * rt_thread(void * arg)
  {
    M3Sds * sys;
    RT_TASK * task;
    SEM * status_sem;
    SEM * command_sem;
    RTUtilBase * rtutil((RTUtilBase*) arg);
    M3UTATorqueShmSdsStatus shm_status;
    M3UTATorqueShmSdsCommand shm_cmd;
    
    jspace::State state(3, 3, 3);
    jspace::Vector command(3);
    RTIME tick_period;
    int cb_status;

    TorqueFeedback* torque_fb(new TorqueFeedback());
    Vector t_offset(3);
    t_offset[0] = 0;//1.55;//1.6125;
    t_offset[1] = 0;//1.71;//1.630;    
    t_offset[2] = 0;//1.50;//1.6375;
    float conv_factor = 1; // 17.76; //1000*(1/482)*(5/3.3)*(1/0.02)*(1/8.85);

    //////////////////////////////////////////////////
    // Initialize shared memory, RT task, and semaphores.
    
    rt_thread_state = RT_THREAD_INIT;
    shutdown_request = 0;
    
    sys = (M3Sds*) rt_shm_alloc(nam2num(TORQUE_SHM), sizeof(M3Sds), USE_VMALLOC);
    if (sys) {
      fprintf(stderr, "found shared memory\n");
    }
    else {
      fprintf(stderr, "rt_shm_alloc failed for %s\n", TORQUE_SHM);
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_sys;
    }
    
    task = rt_task_init_schmod(nam2num("TSHMP"), 0, 0, 0, SCHED_FIFO, 0xF);
    rt_allow_nonroot_hrt();
    if (0 == task) {
      fprintf(stderr, "rt_task_init_schmod failed for TSHMP\n");
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_task;
    }
    
    status_sem = (SEM*) rt_get_adr(nam2num(TORQUE_STATUS_SEM));
    if ( ! status_sem) {
      fprintf(stderr, "semaphore %s not found\n", TORQUE_STATUS_SEM);
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_status_sem;
    }
    
    command_sem = (SEM*) rt_get_adr(nam2num(TORQUE_CMD_SEM));
    if ( ! command_sem) {
      fprintf(stderr, "semaphore %s not found\n", TORQUE_CMD_SEM);
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_command_sem;
    }
    
    //////////////////////////////////////////////////
    // Give the user a chance to do stuff before we enter periodic
    // hard real time.
    
    rt_sem_wait(status_sem);
    memcpy(&shm_status, sys->status, sizeof(shm_status));
    rt_sem_signal(status_sem);

    for (size_t ii(0); ii < 3; ++ii) {
      state.position_[ii] = M_PI * shm_status.mobile_base.theta[ii] / 180.0;
      state.velocity_[ii] = M_PI * shm_status.mobile_base.thetadot[ii] / 180.0;
      state.force_[ii] = conv_factor * (1e-3 * shm_status.mobile_base.torque[ii] - t_offset[ii]);
    }

    for (size_t ii(0); ii < 3; ++ii) {
      state.accelerometer_[ii] = shm_status.mobile_base.imu.accelerometer[ii];
      state.magnetometer_[ii] = shm_status.mobile_base.imu.magnetometer[ii];
      state.ang_vel_[ii] = shm_status.mobile_base.imu.ang_vel[ii];
      for (size_t jj(0); jj < 3; ++jj) {
	state.orientation_mtx_(ii,jj) = shm_status.mobile_base.imu.orientation_mtx[3*ii+jj];
      }
    }

    cb_status = rtutil->init(state);
    if (0 != cb_status) {
      fprintf(stderr, "init callback returned %d\n", cb_status);
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_init_callback;
    }
    
    if (0 >= rt_period_ns) {
      fprintf(stderr, "invalid rt_period_ns %lld\n", rt_period_ns);
      rt_thread_state = RT_THREAD_ERROR;
      goto cleanup_period_check;
    }
    
    //////////////////////////////////////////////////
    // Start the real time engine...
    
    rt_thread_state = RT_THREAD_RUNNING;
    tick_period = nano2count(rt_period_ns);
    rt_task_make_periodic(task, rt_get_time() + tick_period, tick_period); 
    mlockall(MCL_CURRENT | MCL_FUTURE);
    rt_make_hard_real_time();
    
    //////////////////////////////////////////////////
    // The loop.
    
    for (long long step_cnt(0); 0 == shutdown_request; ++step_cnt) {
      
      rt_task_wait_period();
      long long const start_time(nano2count(rt_get_cpu_time_ns()));
      
      rt_sem_wait(status_sem);
      memcpy(&shm_status, sys->status, sizeof(shm_status));
      rt_sem_signal(status_sem);

      for (size_t ii(0); ii < 3; ++ii) {
	state.position_[ii] = M_PI * shm_status.mobile_base.theta[ii] / 180.0;
	state.velocity_[ii] = M_PI * shm_status.mobile_base.thetadot[ii] / 180.0;
	state.force_[ii] = conv_factor * (1e-3 * shm_status.mobile_base.torque[ii] - t_offset[ii]);
      }

      for (size_t ii(0); ii < 3; ++ii) {
	state.accelerometer_[ii] = shm_status.mobile_base.imu.accelerometer[ii];
	state.magnetometer_[ii] = shm_status.mobile_base.imu.magnetometer[ii];
	state.ang_vel_[ii] = shm_status.mobile_base.imu.ang_vel[ii];
	for (size_t jj(0); jj < 3; ++jj) {
	  state.orientation_mtx_(ii,jj) = shm_status.mobile_base.imu.orientation_mtx[3*ii+jj];
	}
      }
      /*
      state.orientation_mtx_(0,0) = cos(0.178);
      state.orientation_mtx_(0,1) = 0;     
      state.orientation_mtx_(0,2) = sin(0.178);
      state.orientation_mtx_(1,0) = 0;
      state.orientation_mtx_(1,1) = 1;
      state.orientation_mtx_(1,2) = 0;
      state.orientation_mtx_(2,0) = -sin(0.178);
      state.orientation_mtx_(2,1) = 0;
      state.orientation_mtx_(2,2) = cos(0.178);*/

      if (0 != cb_status) {
	fprintf(stderr, "update callback returned %d\n", cb_status);
	rt_thread_state = RT_THREAD_ERROR;
	shutdown_request = 1;
	continue;
      }

      cb_status = rtutil->update(state, command);
      if (0 != cb_status) {
	fprintf(stderr, "update callback returned %d\n", cb_status);
	rt_thread_state = RT_THREAD_ERROR;
	shutdown_request = 1;
	continue;
      }
      
      Vector fb_command(torque_fb->computeFeedback(command,state.force_));

      for (size_t ii(0); ii < 3; ++ii) {
	//shm_cmd.mobile_base.tq_desired[ii] = 1.0e3 * fb_command[ii];
	shm_cmd.mobile_base.tq_desired[ii] = 1.0e3 * (command[ii] / 1.8619);
      }

      shm_cmd.timestamp = shm_status.timestamp;
      rt_sem_wait(command_sem);
      memcpy(sys->cmd, &shm_cmd, sizeof(shm_cmd));		
      rt_sem_signal(command_sem);
      
      long long const end_time(nano2count(rt_get_cpu_time_ns()));
      long long const dt(end_time - start_time);
      if (dt > tick_period) {
	cb_status = rtutil->slowdown(step_cnt,
				     count2nano(tick_period),
				     count2nano(dt));
	if (0 != cb_status) {
	  fprintf(stderr, "slowdown callback returned %d\n"
		  "  iteration: %lld\n"
		  "  desired period: %lld ns\n"
		  "  actual period: %lld ns\n",
		  cb_status, step_cnt, count2nano(tick_period), count2nano(dt));
	  rt_thread_state = RT_THREAD_ERROR;
	  shutdown_request = 1;
	  continue;
	}
	fprintf(stderr, "slowing RT task down to %lld ns (instead of %lld ns)\n",
		count2nano(dt), count2nano(tick_period));
	tick_period = dt;
	rt_period_ns = count2nano(dt);
	rt_task_make_periodic(task, rt_get_time() + tick_period, tick_period); 
      }
      
    } // end "the big for loop"
    
    //////////////////////////////////////////////////
    // Clean up after ourselves
    
    fprintf(stderr, "exiting RT thread\n");
    
    rt_thread_state = RT_THREAD_CLEANUP;
    rt_make_soft_real_time();
    
    cb_status = rtutil->cleanup();
    if (0 != cb_status) {
      fprintf(stderr, "cleanup callback returned %d\n", cb_status);
      rt_thread_state = RT_THREAD_ERROR;
    }
    else {
      rt_thread_state = RT_THREAD_DONE;
    }
    
  cleanup_period_check:
  cleanup_init_callback:
  cleanup_command_sem:
  cleanup_status_sem:
    rt_task_delete(task);
  cleanup_task:
    rt_shm_free(nam2num(TORQUE_SHM));
  cleanup_sys:
    
    return 0;
  }
  
  
  RTUtilBase::
  ~RTUtilBase()
  {
  }
  
  
  void RTUtilBase::
  start(long long frequency_hz) throw(std::runtime_error)
  {
    if (nonrt_task) {
      throw std::runtime_error("already running");
    }
    
    rt_period_ns = 1000000000L / frequency_hz;
    long long const rt_period_us(rt_period_ns / 1000);
    
    rt_allow_nonroot_hrt();
    nonrt_task = rt_task_init_schmod(nam2num("TSHM"), RT_TASK_PRIORITY, 0, 0, SCHED_FIFO, 0xF);
    if ( ! nonrt_task) {
      throw std::runtime_error("rt_task_init_schmod failed for non-RT task");
    }
    
    fprintf(stderr, "spawning RT thread");
    rt_thread_state = RT_THREAD_UNDEF;
    rt_thread_id = rt_thread_create((void*) rt_thread,
				    (void*) this,
				    10000); // XXXX 10000 is stack size I think
    for (size_t ii(0); ii < 30; ++ii) {
      fprintf(stderr, ".");
      if ((RT_THREAD_RUNNING == rt_thread_state)
	  || (RT_THREAD_ERROR == rt_thread_state)) {
	break;
      }
      usleep(rt_period_us);
    }
    
    if (RT_THREAD_RUNNING != rt_thread_state) {
      switch (rt_thread_state) {
      case RT_THREAD_UNDEF: fprintf(stderr, "RT_THREAD_UNDEF\n"); break;
      case RT_THREAD_INIT: fprintf(stderr, "RT_THREAD_INIT\n"); break;
      case RT_THREAD_RUNNING: fprintf(stderr, "RT_THREAD_RUNNING\n"); break;
      case RT_THREAD_CLEANUP: fprintf(stderr, "RT_THREAD_CLEANUP\n"); break;
      case RT_THREAD_ERROR: fprintf(stderr, "RT_THREAD_ERROR\n"); break;
      case RT_THREAD_DONE: fprintf(stderr, "RT_THREAD_DONE\n"); break;
      default: fprintf(stderr, "invalid state %d\n", rt_thread_state);
      }
      shutdown_request = 1;
      usleep(15 * rt_period_us);
      rt_task_delete(nonrt_task);
      rt_thread_join(rt_thread_id);
      nonrt_task = 0;
      rt_thread_id = 0;
      throw std::runtime_error("RT thread failed to start");
    }
    
    fprintf(stderr, "OK\n");
  }
  
  
  rt_thread_state_t RTUtilBase::
  getState()
  {
    return rt_thread_state;
  }
  
  
  rt_thread_state_t RTUtilBase::
  shutdown()
  {
    if ( ! nonrt_task) {
      fprintf(stderr, "no RT thread to shut down");
      return RT_THREAD_DONE;
    }
    
    fprintf(stderr, "shutting down RT thread");
    shutdown_request = 1;
    long long const rt_period_us(rt_period_ns / 1000);
    for (size_t ii(0); ii < 30; ++ii) {
      fprintf(stderr, ".");
      if (RT_THREAD_RUNNING != rt_thread_state) {
	break;
      }
      usleep(rt_period_us);
    }
    
    if (RT_THREAD_RUNNING == rt_thread_state) {
      fprintf(stderr, "shut down timed out\n");
      rt_thread_state = RT_THREAD_ERROR;
    }
    else {
      fprintf(stderr, "OK\n");
    }
    
    rt_task_delete(nonrt_task);
    rt_thread_join(rt_thread_id);
    nonrt_task = 0;
    rt_thread_id = 0;
    
    return rt_thread_state;
  }
  
}

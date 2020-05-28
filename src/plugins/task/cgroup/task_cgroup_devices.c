/***************************************************************************** \
 *  task_cgroup_devices.c - devices cgroup subsystem for task/cgroup
 *****************************************************************************
 *  Copyright (C) 2011 BULL
 *  Written by Yiannis Georgiou <yiannis.georgiou@bull.fr>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE
#include <glob.h>
#include <limits.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef MAJOR_IN_MKDEV
#  include <sys/mkdev.h>
#endif
#ifdef MAJOR_IN_SYSMACROS
#  include <sys/sysmacros.h>
#endif

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/xstring.h"
#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/slurmd/common/xcpuinfo.h"
#include "src/slurmd/slurmd/slurmd.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"

#include "task_cgroup.h"

static char user_cgroup_path[PATH_MAX];
static char job_cgroup_path[PATH_MAX];
static char jobstep_cgroup_path[PATH_MAX];
static char cgroup_allowed_devices_file[PATH_MAX];

static xcgroup_ns_t devices_ns;

static xcgroup_t user_devices_cg;
static xcgroup_t job_devices_cg;
static xcgroup_t step_devices_cg;

static void _calc_device_major(char *dev_path[PATH_MAX],
			       char *dev_major[PATH_MAX],
			       int lines);

static int _read_allowed_devices_file(char *allowed_devices[PATH_MAX]);


typedef struct
{
    char fake_device_path[128];
    char bus_id[128];
} lancium_device_mapping_t;

lancium_device_mapping_t *mapping;
int mapping_cnt = 0;

extern void lancium_get_all_nvidia_bus_ids(List pci_list)
{
	//now we need to find the pci bus
	FILE *fp;
	char *res;
	char *cmd = "ls /proc/driver/nvidia/gpus 2>&1";

	/* Open the command for reading. */
	fp = popen(cmd, "r");
	if (fp == NULL)
	{
		debug("lancium: could not find nvidia bus_ids");
	}

	debug("lancium: attempting to get the nvidia pci buses-----------");
	// Read the output a line at a time
	while (fgets(res, 128 * sizeof(char), fp) != NULL)
	{
		if(res[4] == ':' && res[7] == ':' && res[10] == '.')
		{
			res = strtok(res, "\n"); //remove new line char

			//we need to make a copy
			char* scpy = malloc(128 * sizeof(char)); //this is freed when removed from the list
			strncpy(scpy, res, 128);

			list_append(pci_list, scpy);
			debug("lancium: found bus id %s", res);
		}
		else
		{
			debug("lancium: no nvidia driver information found");
		}
	}
	debug("lancium: end of found nvidia pci buses--------------------");

	/* close */
	pclose(fp);
}

extern void lancium_find_dev_path_from_bus(char* dev_path_out, int max_out_length, char* bus)
{
	FILE *fp;
	char res[128];
	char cmd[128];

	strcpy(cmd, "cat /proc/driver/nvidia/gpus/");
	strncat(cmd, bus, 93); //should never get close, but protect the buffer
	strcat(cmd, "/information 2>&1");

	/* Open the command for reading. */
	fp = popen(cmd, "r");
	if (fp == NULL)
	{
		debug("lancium: could not find nvidia device information about bus_id=%s", bus);
	}

	debug("lancium: attempting to get the device minor number for device with bus_id=%s", bus);
	// Read the output a line at a time; instead of skipping to line 9, we doing this in case nvidia changes stuff in their output
	while (fgets(res, sizeof(res), fp) != NULL)
	{
		char* start = strtok(res, "\t\n");

		if(strcmp(start, "Device Minor: ") == 0)
		{
			//we have found the right line, get the dev_num
			char* dnumStr = strtok(NULL, "\t\n");

			//there appears to be a space infront of the dev_num, remove it
			if(dnumStr[0] == ' ')
				dnumStr++;

			debug("lancium: we have found the current device_num=%s for bus_id=%s", dnumStr, bus);

			//set input var
			strcpy(dev_path_out, "/dev/nvidia");
			strncat(dev_path_out, dnumStr, max_out_length-13); //protect buffer
		}
	}

	/* close */
	pclose(fp);
}

void __lancium_del_node(void* str)
{
	free(str);
}

extern int task_cgroup_devices_init(slurm_cgroup_conf_t *slurm_cgroup_conf)
{
	uint16_t cpunum;
	FILE *file = NULL;

	/* initialize cpuinfo internal data */
	if (xcpuinfo_init() != XCPUINFO_SUCCESS)
		return SLURM_ERROR;

	/* initialize user/job/jobstep cgroup relative paths */
	user_cgroup_path[0] = '\0';
	job_cgroup_path[0] = '\0';
	jobstep_cgroup_path[0] = '\0';
	/* initialize allowed_devices_filename */
	cgroup_allowed_devices_file[0] = '\0';

	if (get_procs(&cpunum) != 0) {
		error("task/cgroup: unable to get a number of CPU");
		goto error;
	}

	if ((strlen(slurm_cgroup_conf->allowed_devices_file) + 1) >= PATH_MAX) {
		error("task/cgroup: device file path length exceeds limit: %s",
		      slurm_cgroup_conf->allowed_devices_file);
		goto error;
	}
	strcpy(cgroup_allowed_devices_file,
	       slurm_cgroup_conf->allowed_devices_file);
	if (xcgroup_ns_create(slurm_cgroup_conf, &devices_ns, "", "devices")
	    != XCGROUP_SUCCESS ) {
		error("task/cgroup: unable to create devices namespace");
		goto error;
	}

	file = fopen(cgroup_allowed_devices_file, "r");
	if (!file) {
		fatal("task/cgroup: %s doesn't exist, this is needed for proper functionality when Constraining Devices.",
		      cgroup_allowed_devices_file);
		goto error;
	} else
		fclose(file);

	/////////////////// LANCIUM INIT ///////////////////////////////////////////////////////////////////

	List gres_list = list_create(NULL);
	lancium_gres_plugin_get_all_gres(gres_list);

	List pci_list = list_create(__lancium_del_node);
	lancium_get_all_nvidia_bus_ids(pci_list);

	debug("lancium: about to map the fake devices to bus ids");

	int gres_cnt = list_count(gres_list);

	if(gres_cnt > list_count(pci_list))
	{
		error("lancium: gres list size is larger than the number of gpus on the system! This is unexpected and not currently handled. Have you added gres resources besides GPUs? \
			If so, you need to revist the slurm plugin changes.");
	}

	mapping_cnt = gres_cnt;

	//alocate space for the mapping
	mapping = malloc(mapping_cnt * sizeof(lancium_device_mapping_t)); //needs to be released in fini

	//iterate list and search for our fake_device to create maps to real device bus ids
	gres_device_t *gres_device;
	ListIterator dev_itr = list_iterator_create(gres_list);
	char *bus;

	while ((gres_device = list_next(dev_itr)))
	{
		char output[128];
		strcpy(output, "lancium: we are in task_cgroup_devices_init and found fake device=");
		strncat(output, gres_device->path, 14);
		debug("%s", output);

		//get a bus_id from the list
		bus = list_pop(pci_list);
		
		debug("lancium: we are mapping this to the pci_bus %s", bus);

		//get an index and check its bounds
		int index = gres_device->dev_num;

		if(index >= mapping_cnt || index < 0)
		{
			fatal_abort("lancium: we appear to have made an invalid assumption that device numbers are usuable as index. We need to exit otherwise we will write out of bounds. Info => device: %s dev_num/index: %d", gres_device->path, index);
		}

		debug("lancium: index is=%d", index);

		//assign a consistant mapping
		strncpy(mapping[index].fake_device_path, gres_device->path, 128);
		strncpy(mapping[index].bus_id, bus, 128);
	}
	list_iterator_destroy(dev_itr);

	//this should destroy the allocated string inside pci_list
	list_destroy(pci_list);

	//////////////////////////////////////////////////////////////////////////////////////////////////

	return SLURM_SUCCESS;

error:
	xcgroup_ns_destroy(&devices_ns);
	xcpuinfo_fini();
	return SLURM_ERROR;
}

extern int task_cgroup_devices_fini(slurm_cgroup_conf_t *slurm_cgroup_conf)
{
	xcgroup_t devices_cg;

	/* Similarly to task_cgroup_{memory,cpuset}_fini(), we must lock the
	 * root cgroup so we don't race with another job step that is
	 * being started.  */
        if (xcgroup_create(&devices_ns, &devices_cg,"",0,0)
	    == XCGROUP_SUCCESS) {
                if (xcgroup_lock(&devices_cg) == XCGROUP_SUCCESS) {
			/* First move slurmstepd to the root devices cg
			 * so we can remove the step/job/user devices
			 * cg's.  */
			xcgroup_move_process(&devices_cg, getpid());

			xcgroup_wait_pid_moved(&step_devices_cg,
					       "devices step");

			if (xcgroup_delete(&step_devices_cg) != SLURM_SUCCESS)
                                debug2("task/cgroup: unable to remove step "
                                       "devices : %m");
                        if (xcgroup_delete(&job_devices_cg) != XCGROUP_SUCCESS)
                                debug2("task/cgroup: not removing "
                                       "job devices : %m");
                        if (xcgroup_delete(&user_devices_cg)
			    != XCGROUP_SUCCESS)
                                debug2("task/cgroup: not removing "
                                       "user devices : %m");
                        xcgroup_unlock(&devices_cg);
                } else
                        error("task/cgroup: unable to lock root devices : %m");
                xcgroup_destroy(&devices_cg);
        } else
                error("task/cgroup: unable to create root devices : %m");

	if ( user_cgroup_path[0] != '\0' )
		xcgroup_destroy(&user_devices_cg);
	if ( job_cgroup_path[0] != '\0' )
		xcgroup_destroy(&job_devices_cg);
	if ( jobstep_cgroup_path[0] != '\0' )
		xcgroup_destroy(&step_devices_cg);

	user_cgroup_path[0] = '\0';
	job_cgroup_path[0] = '\0';
	jobstep_cgroup_path[0] = '\0';

	cgroup_allowed_devices_file[0] = '\0';

	xcgroup_ns_destroy(&devices_ns);

	/////////////////// LANCIUM CLEANUP ///////////////////////////////////////////////////////////////////

	free(mapping);

	//////////////////////////////////////////////////////////////////////////////////////////////////

	xcpuinfo_fini();
	return SLURM_SUCCESS;
}

extern int task_cgroup_devices_create(stepd_step_rec_t *job)
{
	int k, rc, allow_lines = 0;
	int fstatus = SLURM_ERROR;
	char *allowed_devices[PATH_MAX], *allowed_dev_major[PATH_MAX];
	xcgroup_t devices_cg;
	uint32_t jobid = job->jobid;
	uint32_t stepid = job->stepid;
	uid_t uid = job->uid;
	uid_t gid = job->gid;

	List job_gres_list = job->job_gres_list;
	List step_gres_list = job->step_gres_list;
	List device_list = NULL;
	ListIterator itr;
	gres_device_t *gres_device;

	char* slurm_cgpath ;

	/* create slurm root cgroup in this cgroup namespace */
	slurm_cgpath = task_cgroup_create_slurm_cg(&devices_ns);
	if (slurm_cgpath == NULL)
		return SLURM_ERROR;

	/* build user cgroup relative path if not set (should not be) */
	if (*user_cgroup_path == '\0') {
		if (snprintf(user_cgroup_path, PATH_MAX, "%s/uid_%u",
			     slurm_cgpath, uid) >= PATH_MAX) {
			error("unable to build uid %u cgroup relative path : %m",
			      uid);
			xfree(slurm_cgpath);
			return SLURM_ERROR;
		}
	}
	xfree(slurm_cgpath);

	/* build job cgroup relative path if no set (should not be) */
	if (*job_cgroup_path == '\0') {
		if (snprintf(job_cgroup_path, PATH_MAX, "%s/job_%u",
			     user_cgroup_path, jobid) >= PATH_MAX) {
			error("task/cgroup: unable to build job %u devices "
			      "cgroup relative path : %m", jobid);
			return SLURM_ERROR;
		}
	}

	/* build job step cgroup relative path (should not be) */
	if (*jobstep_cgroup_path == '\0') {
		int cc;
		if (stepid == SLURM_BATCH_SCRIPT) {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				      "%s/step_batch", job_cgroup_path);
		} else if (stepid == SLURM_EXTERN_CONT) {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				      "%s/step_extern", job_cgroup_path);
		} else {
			cc = snprintf(jobstep_cgroup_path, PATH_MAX,
				     "%s/step_%u",
				     job_cgroup_path, stepid);
		}
		if (cc >= PATH_MAX) {
			error("task/cgroup: unable to build job step %u.%u "
			      "devices cgroup relative path : %m",
			      jobid, stepid);
			return SLURM_ERROR;
		}
	}

	/*
	 * create devices root cgroup and lock it
	 *
	 * we will keep the lock until the end to avoid the effect of a release
	 * agent that would remove an existing cgroup hierarchy while we are
	 * setting it up. As soon as the step cgroup is created, we can release
	 * the lock.
	 * Indeed, consecutive slurm steps could result in cgroup being removed
	 * between the next EEXIST instanciation and the first addition of
	 * a task. The release_agent will have to lock the root devices cgroup
	 * to avoid this scenario.
	 */
	if (xcgroup_create(&devices_ns, &devices_cg, "", 0, 0) !=
	    XCGROUP_SUCCESS ) {
		error("task/cgroup: unable to create root devices cgroup");
		return SLURM_ERROR;
	}
	if (xcgroup_lock(&devices_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&devices_cg);
		error("task/cgroup: unable to lock root devices cgroup");
		return SLURM_ERROR;
	}

	debug2("task/cgroup: manage devices jor job '%u'", jobid);

	/*
	 * create user cgroup in the devices ns (it could already exist)
	 */
	if (xcgroup_create(&devices_ns, &user_devices_cg, user_cgroup_path,
			   getuid(), getgid()) != XCGROUP_SUCCESS) {
		goto error;
	}
	if (xcgroup_instantiate(&user_devices_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&user_devices_cg);
		goto error;
	}

	/* TODO
	 * check that user's devices cgroup is consistant and allow the
	 * appropriate devices
	 */


	/*
	 * create job cgroup in the devices ns (it could already exist)
	 */
	if (xcgroup_create(&devices_ns, &job_devices_cg, job_cgroup_path,
			    getuid(), getgid()) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&user_devices_cg);
		goto error;
	}
	if (xcgroup_instantiate(&job_devices_cg) != XCGROUP_SUCCESS) {
		xcgroup_destroy(&user_devices_cg);
		xcgroup_destroy(&job_devices_cg);
		goto error;
	}

	/*
         * create the entry with major minor for the default allowed devices
         * read from the file
         */
	allow_lines = _read_allowed_devices_file(allowed_devices);
	_calc_device_major(allowed_devices, allowed_dev_major, allow_lines);
	for (k = 0; k < allow_lines; k++)
		xfree(allowed_devices[k]);

	/*
	 * with the current cgroup devices subsystem design (whitelist only
	 * supported) we need to allow all different devices that are supposed
	 * to be allowed by* default.
	 */
	for (k = 0; k < allow_lines; k++) {
		debug2("Default access allowed to device %s for job",
		       allowed_dev_major[k]);
		xcgroup_set_param(&job_devices_cg, "devices.allow",
				  allowed_dev_major[k]);
	}

	/*
         * allow or deny access to devices according to job GRES permissions
         */
	device_list = gres_plugin_get_allocated_devices(job_gres_list, true);

	if (device_list) {
		itr = list_iterator_create(device_list);
		while ((gres_device = list_next(itr))) {

			//////////////////////////// LANCIUM MODIFICATION //////////////////////////////////////////

			int cur_fake_dev_num = gres_device->dev_num;

			debug("lancium: about to use device mapping for cgroup for fake device=%s", gres_device->path);

			//confirm index/device match
			if(strcmp(mapping[cur_fake_dev_num].fake_device_path, gres_device->path) != 0)
			{
				error("lancium: something is wrong with the device mapping initilization!");
			}

			//find the real device we want (bus_id)
			char* desired_bus = mapping[cur_fake_dev_num].bus_id;

			debug("lancium: this device has mapped to bus_id=%s", desired_bus);

			//now we need to find what device minor number this cards is actually at now
			char cur_real_dev_path[64];
			lancium_find_dev_path_from_bus(cur_real_dev_path, 64, desired_bus);

			debug("lancium: this device has mapped to dev_path=%s", cur_real_dev_path);

			//get major from path
			char* cur_real_major = gres_device_major(cur_real_dev_path);

			debug("lancium: this device has mapped to dev_major=%s", cur_real_major);

			if (gres_device->alloc) {
				debug("lancium: Allowing access to device %s(%s) for job",
				      cur_real_major, cur_real_dev_path);
				xcgroup_set_param(&job_devices_cg,
						  "devices.allow",
						  cur_real_major);
			} else {
				debug("lancium: Not allowing access to device %s(%s) for job",
				       cur_real_major, cur_real_dev_path);
				xcgroup_set_param(&job_devices_cg,
						  "devices.deny",
						  cur_real_major);
			}

			////////////////////////////////////////////////////////////////////////////////////////////
		}
		list_iterator_destroy(itr);
		list_destroy(device_list);
	}

	/*
	 * create step cgroup in the devices ns (it should not exists)
	 * use job's user uid/gid to enable tasks cgroups creation by
	 * the user inside the step cgroup owned by root
	 */
	if (xcgroup_create(&devices_ns, &step_devices_cg, jobstep_cgroup_path,
			   uid, gid) != XCGROUP_SUCCESS ) {
		/* do not delete user/job cgroup as */
		/* they can exist for other steps */
		xcgroup_destroy(&user_devices_cg);
		xcgroup_destroy(&job_devices_cg);
		goto error;
	}
	if ( xcgroup_instantiate(&step_devices_cg) != XCGROUP_SUCCESS ) {
		xcgroup_destroy(&user_devices_cg);
		xcgroup_destroy(&job_devices_cg);
		xcgroup_destroy(&step_devices_cg);
		goto error;
	}


	if ((job->stepid != SLURM_BATCH_SCRIPT) &&
	    (job->stepid != SLURM_EXTERN_CONT)) {
		/*
		 * with the current cgroup devices subsystem design (whitelist
		 * only supported) we need to allow all different devices that
		 * are supposed to be allowed by default.
		 */
		for (k = 0; k < allow_lines; k++) {
			debug2("Default access allowed to device %s for step",
			     allowed_dev_major[k]);
			xcgroup_set_param(&step_devices_cg, "devices.allow",
					  allowed_dev_major[k]);
		}

		/*
		 * allow or deny access to devices according to GRES permissions
		 * for the step
		 */
		device_list = gres_plugin_get_allocated_devices(
			step_gres_list, false);

		if (device_list) {
			itr = list_iterator_create(device_list);
			while ((gres_device = list_next(itr))) {

			//////////////////////////// LANCIUM MODIFICATION //////////////////////////////////////////

			int cur_fake_dev_num = gres_device->dev_num;

			debug("lancium: about to use device mapping for cgroup for fake device=%s", gres_device->path);

			//confirm index/device match
			if(strcmp(mapping[cur_fake_dev_num].fake_device_path, gres_device->path) != 0)
			{
				error("lancium: something is wrong with the device mapping initilization!");
			}

			//find the real device we want (bus_id)
			char* desired_bus = mapping[cur_fake_dev_num].bus_id;

			debug("lancium: this device has mapped to bus_id=%s", desired_bus);

			//now we need to find what device minor number this cards is actually at now
			char cur_real_dev_path[64];
			lancium_find_dev_path_from_bus(cur_real_dev_path, 64, desired_bus);

			debug("lancium: this device has mapped to dev_path=%s", cur_real_dev_path);

			//get major from path
			char* cur_real_major = gres_device_major(cur_real_dev_path);

			debug("lancium: this device has mapped to dev_major=%s", cur_real_major);

			if (gres_device->alloc) {
				debug("lancium: Allowing access to device %s(%s) for job",
				      cur_real_major, cur_real_dev_path);
				xcgroup_set_param(&step_devices_cg,
						  "devices.allow",
						  cur_real_major);
			} else {
				debug("lancium: Not allowing access to device %s(%s) for job",
				       cur_real_major, cur_real_dev_path);
				xcgroup_set_param(&step_devices_cg,
						  "devices.deny",
						  cur_real_major);
			}

			////////////////////////////////////////////////////////////////////////////////////////////

			}
			list_iterator_destroy(itr);
			list_destroy(device_list);
		}
	}
	/* attach the slurmstepd to the step devices cgroup */
	pid_t pid = getpid();
	rc = xcgroup_add_pids(&step_devices_cg, &pid, 1);
	if (rc != XCGROUP_SUCCESS) {
		error("task/cgroup: unable to add slurmstepd to devices cg '%s'",
		      step_devices_cg.path);
		fstatus = SLURM_ERROR;
	} else {
		fstatus = SLURM_SUCCESS;
	}

error:
	xcgroup_unlock(&devices_cg);
	xcgroup_destroy(&devices_cg);
	for (k = 0; k < allow_lines; k++) {
		xfree(allowed_dev_major[k]);
	}
	xfree(allowed_dev_major);

	return fstatus;
}

extern int task_cgroup_devices_attach_task(stepd_step_rec_t *job)
{
	int fstatus = SLURM_ERROR;

	/* tasks are automatically attached as slurmstepd is in the step cg */
	fstatus = SLURM_SUCCESS;

	return fstatus;
}

static void _calc_device_major(char *dev_path[PATH_MAX],
			       char *dev_major[PATH_MAX],
			       int lines)
{

	int k;

	if (lines > PATH_MAX) {
		error("task/cgroup: more devices configured than table size "
		      "(%d > %d)", lines, PATH_MAX);
		lines = PATH_MAX;
	}
	for (k = 0; k < lines; k++)
		dev_major[k] = gres_device_major(dev_path[k]);
}


static int _read_allowed_devices_file(char **allowed_devices)
{

	FILE *file = fopen(cgroup_allowed_devices_file, "r");
	int i, l, num_lines = 0;
	char line[256];
	glob_t globbuf;

	for( i=0; i<256; i++ )
		line[i] = '\0';

	if ( file != NULL ){
		while (fgets(line, sizeof(line), file)) {
			line[strlen(line)-1] = '\0';

			/* global pattern matching and return the list of matches*/
			if (glob(line, GLOB_NOSORT, NULL, &globbuf)) {
				debug3("Device %s does not exist", line);
			} else {
				for (l=0; l < globbuf.gl_pathc; l++) {
					allowed_devices[num_lines] =
						xstrdup(globbuf.gl_pathv[l]);
					num_lines++;
				}
				globfree(&globbuf);
			}
		}
		fclose(file);
	}
	else
		fatal("%s: %s does not exist, please create this file.",
		      __func__, cgroup_allowed_devices_file);

	return num_lines;
}


extern int task_cgroup_devices_add_pid(pid_t pid)
{
	return xcgroup_add_pids(&step_devices_cg, &pid, 1);
}

#define _GNU_SOURCE
#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mount.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <yajl/yajl_tree.h>

#include "config.h"

#include <libmount/libmount.h>

#define _cleanup_(x) __attribute__((cleanup(x)))

static inline void freep(void *p) {
	free(*(void**) p);
}

static inline void closep(int *fd) {
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static inline void fclosep(FILE **fp) {
	if (*fp)
		fclose(*fp);
	*fp = NULL;
}

static inline void mnt_free_iterp(struct libmnt_iter **itr) {
	if (*itr)
		mnt_free_iter(*itr);
	*itr=NULL;
}

static inline void mnt_free_fsp(struct libmnt_fs **itr) {
	if (*itr)
		mnt_free_fs(*itr);
	*itr=NULL;
}

#define _cleanup_free_ _cleanup_(freep)
#define _cleanup_close_ _cleanup_(closep)
#define _cleanup_fclose_ _cleanup_(fclosep)
#define _cleanup_mnt_iter_ _cleanup_(mnt_free_iterp)
#define _cleanup_mnt_fs_ _cleanup_(mnt_free_fsp)

#define DEFINE_CLEANUP_FUNC(type, func)                         \
	static inline void func##p(type *p) {                   \
		if (*p)                                         \
			func(*p);                               \
	}                                                       \

DEFINE_CLEANUP_FUNC(yajl_val, yajl_tree_free)

#define pr_perror(fmt, ...) syslog(LOG_ERR, "custodiahook <error>: " fmt ": %m\n", ##__VA_ARGS__)
#define pr_pinfo(fmt, ...) syslog(LOG_INFO, "custodiahook <info>: " fmt "\n", ##__VA_ARGS__)
#define pr_pdebug(fmt, ...) syslog(LOG_DEBUG, "custodiahook <debug>: " fmt "\n", ##__VA_ARGS__)

#define BUFLEN 1024
#define CHUNKSIZE 4096

static int makepath(char *dir, mode_t mode)
{
    if (!dir) {
	errno = EINVAL;
	return -1;
    }

    if (strlen(dir) == 1 && dir[0] == '/')
	return 0;

    makepath(dirname(strdupa(dir)), mode);

    return mkdir(dir, mode);
}

static int bind_mount(const char *src, const char *dest, int readonly) {
	if (mount(src, dest, "bind", MS_BIND, NULL) == -1) {
		pr_perror("Failed to mount %s on %s", src, dest);
		return -1;
	}
	//  Remount bind mount to read/only if requested by the caller
	if (readonly) {
		if (mount(src, dest, "bind", MS_REMOUNT|MS_BIND|MS_RDONLY, "") == -1) {
			pr_perror("Failed to remount %s readonly", dest);
			return -1;
		}
	}
	return 0;
}

static bool contains_mount(const char **config_mounts, unsigned len, const char *mount) {
	for (unsigned i = 0; i < len; i++) {
		if (!strcmp(mount, config_mounts[i])) {
			pr_pdebug("%s already present as a mount point in container configuration, skipping\n", mount);
			return true;
		}
	}
	return false;
}

static int prestart(const char *rootfs,
		int pid,
		const char **config_mounts,
		unsigned config_mounts_len)
{

	_cleanup_close_  int fd = -1;
	_cleanup_free_   char *options = NULL;

	char process_mnt_ns_fd[PATH_MAX];
	snprintf(process_mnt_ns_fd, PATH_MAX, "/proc/%d/ns/mnt", pid);

	fd = open(process_mnt_ns_fd, O_RDONLY);
	if (fd < 0) {
		pr_perror("Failed to open mnt namespace fd %s", process_mnt_ns_fd);
		return -1;
	}

	/* Join the mount namespace of the target process */
	if (setns(fd, 0) == -1) {
		pr_perror("Failed to setns to %s", process_mnt_ns_fd);
		return -1;
	}
	close(fd);
	fd = -1;

	static char *CUSTODIA_DIR = "/var/run/custodia";
	static char *CUSTODIA_SOCKET = "custodia.sock";
	char source_path[PATH_MAX];
	snprintf(source_path, PATH_MAX, "%s/%s", CUSTODIA_DIR, CUSTODIA_SOCKET);

	struct stat stat_buf;
	if (stat(source_path, &stat_buf) != -1
		&& !contains_mount(config_mounts, config_mounts_len, source_path)) {
		char dest_dir[PATH_MAX];
		snprintf(dest_dir, PATH_MAX, "%s%s", rootfs, CUSTODIA_DIR);
		makepath(dest_dir, 0755);

		char dest_path[PATH_MAX];
		snprintf(dest_path, PATH_MAX, "%s/%s", dest_dir, CUSTODIA_SOCKET);
		open(dest_path, O_CREAT|O_RDONLY|O_CLOEXEC, 0755);

		pr_pinfo("Will try to mount %s on %s", source_path, dest_path);
		if (bind_mount(source_path, dest_path, true) == -1) {
			pr_perror("Failed to bind mount %s on %s", source_path, dest_path);
			return -1;
		}
	}

	return 0;
}

/*
 * Read the entire content of stream pointed to by 'from' into a buffer in memory.
 * Return a pointer to the resulting NULL-terminated string.
 */
char *getJSONstring(FILE *from, size_t chunksize, char *msg)
{
	struct stat stat_buf;
	char *err = NULL, *JSONstring = NULL;
	size_t nbytes, bufsize;

	if (fstat(fileno(from), &stat_buf) == -1) {
		err = "fstat failed";
		goto fail;
	}

	if (S_ISREG(stat_buf.st_mode)) {
		/*
		 * If 'from' is a regular file, allocate a buffer based
		 * on the file size and read the entire content with a
		 * single fread() call.
		 */
		if (stat_buf.st_size == 0) {
			err = "is empty";
			goto fail;
		}

		bufsize = (size_t)stat_buf.st_size;

		JSONstring = (char *)malloc(bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}

		nbytes = fread((void *)JSONstring, 1, (size_t)bufsize, from);
		if (nbytes != (size_t)bufsize) {
			err = "error encountered on read";
			goto fail;
		}
	} else {
		/*
		 * If 'from' is not a regular file, call fread() iteratively
		 * to read sections of 'chunksize' bytes until EOF is reached.
		 * Call realloc() during each iteration to expand the buffer
		 * as needed.
		 */
		bufsize = 0;

		for (;;) {
			JSONstring = (char *)realloc((void *)JSONstring, bufsize + chunksize);
			if (JSONstring == NULL) {
				err = "failed to allocate buffer";
				goto fail;
			}

			nbytes = fread((void *)&JSONstring[bufsize], 1, (size_t)chunksize, from);
			bufsize += nbytes;

			if (nbytes != (size_t)chunksize) {
				if (ferror(from)) {
					err = "error encountered on read";
					goto fail;
				}
				if (feof(from))
					break;
			}
		}

		if (bufsize == 0) {
			err = "is empty";
			goto fail;
		}

		JSONstring = (char *)realloc((void *)JSONstring, bufsize + 1);
		if (JSONstring == NULL) {
			err = "failed to allocate buffer";
			goto fail;
		}
	}

	/* make sure the string is NULL-terminated */
	JSONstring[bufsize] = 0;
	return JSONstring;
fail:
	free(JSONstring);
	pr_perror("%s: %s", msg, err);
	return NULL;
}

int main(int argc, char *argv[])
{
	_cleanup_(yajl_tree_freep) yajl_val node = NULL;
	_cleanup_(yajl_tree_freep) yajl_val config_node = NULL;
	char errbuf[BUFLEN];
	char *stateData;
	char *configData;
	char config_file_name[PATH_MAX];
	_cleanup_fclose_ FILE *fp = NULL;

	/* Read the entire state from stdin */
	snprintf(errbuf, BUFLEN, "failed to read state data from standard input");
	stateData = getJSONstring(stdin, (size_t)CHUNKSIZE, errbuf);
	if (stateData == NULL)
		return EXIT_FAILURE;

	/* Parse the state */
	memset(errbuf, 0, BUFLEN);
	node = yajl_tree_parse((const char *)stateData, errbuf, sizeof(errbuf));
	if (node == NULL) {
		pr_perror("parse_error: ");
		if (strlen(errbuf)) {
			pr_perror(" %s", errbuf);
		} else {
			pr_perror("unknown error");
		}
		return EXIT_FAILURE;
	}

	/* Extract values from the state json */
	const char *root_path[] = { "root", (const char *)0 };
	yajl_val v_root = yajl_tree_get(node, root_path, yajl_t_string);
	if (!v_root) {
		pr_perror("root not found in state");
		return EXIT_FAILURE;
	}
	char *rootfs = YAJL_GET_STRING(v_root);

	const char *pid_path[] = { "pid", (const char *) 0 };
	yajl_val v_pid = yajl_tree_get(node, pid_path, yajl_t_number);
	if (!v_pid) {
		pr_perror("pid not found in state");
		return EXIT_FAILURE;
	}
	int target_pid = YAJL_GET_INTEGER(v_pid);

	const char *id_path[] = { "id", (const char *)0 };
	yajl_val v_id = yajl_tree_get(node, id_path, yajl_t_string);
	if (!v_id) {
		pr_perror("id not found in state");
		return EXIT_FAILURE;
	}
	char *id = YAJL_GET_STRING(v_id);

	/* 'bundlePath' must be specified for the OCI hooks, and from there we read the configuration file */
	const char *bundle_path[] = { "bundlePath", (const char *)0 };
	yajl_val v_bundle_path = yajl_tree_get(node, bundle_path, yajl_t_string);
	if (v_bundle_path) {
		snprintf(config_file_name, PATH_MAX, "%s/config.json", YAJL_GET_STRING(v_bundle_path));
		fp = fopen(config_file_name, "r");
	} else {
		char msg[] = "bundlePath not found in state";
		snprintf(config_file_name, PATH_MAX, "%s", msg);
	}

	if (fp == NULL) {
		pr_perror("Failed to open config file: %s", config_file_name);
		return EXIT_FAILURE;
	}

	/* Read the entire config file */
	snprintf(errbuf, BUFLEN, "failed to read config data from %s", config_file_name);
	configData = getJSONstring(fp, (size_t)CHUNKSIZE, errbuf);
	if (configData == NULL)
		return EXIT_FAILURE;

	/* Parse the config file */
	memset(errbuf, 0, BUFLEN);
	config_node = yajl_tree_parse((const char *)configData, errbuf, sizeof(errbuf));
	if (config_node == NULL) {
		pr_perror("parse_error: ");
		if (strlen(errbuf)) {
			pr_perror(" %s", errbuf);
		} else {
			pr_perror("unknown error");
		}
		return EXIT_FAILURE;
	}

	const char **config_mounts = NULL;
	unsigned config_mounts_len = 0;

	const char *mount_points_path[] = {"mounts", (const char *)0 };
	yajl_val v_mounts = yajl_tree_get(config_node, mount_points_path, yajl_t_array);
	if (!v_mounts) {
		pr_perror("mounts not found in config");
		return EXIT_FAILURE;
	}

	config_mounts_len = YAJL_GET_ARRAY(v_mounts)->len;
	config_mounts = malloc (sizeof(char *) * (config_mounts_len + 1));
	if (! config_mounts) {
		pr_perror("error malloc'ing");
		return EXIT_FAILURE;
	}

	for (unsigned int i = 0; i < config_mounts_len; i++) {
		yajl_val v_mounts_values = YAJL_GET_ARRAY(v_mounts)->values[i];

		const char *destination_path[] = {"destination", (const char *)0 };
		yajl_val v_destination = yajl_tree_get(v_mounts_values, destination_path, yajl_t_string);
		if (!v_destination) {
			pr_perror("Cannot find mount destination");
			return EXIT_FAILURE;
		}
		config_mounts[i] = YAJL_GET_STRING(v_destination);
	}

	const char *args_path[] = {"process", "args", (const char *)0 };
	yajl_val v_args = yajl_tree_get(config_node, args_path, yajl_t_array);
	if (!v_args) {
		pr_perror("args not found in config");
		return EXIT_FAILURE;
	}

	const char *envs[] = {"process", "env", (const char *)0 };
	yajl_val v_envs = yajl_tree_get(config_node, envs, yajl_t_array);
	if (v_envs) {
		for (unsigned int i = 0; i < YAJL_GET_ARRAY(v_envs)->len; i++) {
			yajl_val v_env = YAJL_GET_ARRAY(v_envs)->values[i];
			char *str = YAJL_GET_STRING(v_env);
			if (strncmp (str, "container_uuid=", strlen ("container_uuid=")) == 0) {
				id = strdup (str + strlen ("container_uuid="));
				/* systemd expects $container_uuid= to be an UUID but then treat it as
				   not containing any '-'.  Do the same here.  */
				char *to = id;
				for (char *from = to; *from; from++) {
					if (*from != '-')
						*to++ = *from;
				}
				*to = '\0';
			}
		}
	}

	/* OCI hooks set target_pid to 0 on poststop, as the container process already
	   exited.  If target_pid is bigger than 0 then it is the prestart hook.  */
	if ((argc > 2 && !strcmp("prestart", argv[1])) || target_pid) {
		if (prestart(rootfs, target_pid, config_mounts, config_mounts_len) != 0) {
			return EXIT_FAILURE;
		}
	} else if ((argc > 2 && !strcmp("poststop", argv[1])) || (target_pid == 0)) {
		/* skip poststop */
	} else {
		pr_perror("command not recognized: %s", argv[1]);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

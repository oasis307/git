#include "cache.h"
#include "builtin.h"
#include "dir.h"
#include "parse-options.h"
#include "run-command.h"
#include "refs.h"
#include "sigchain.h"

static const char * const worktree_usage[] = {
	N_("git worktree add [<options>] <path> [<checkout-options>] <branch>"),
	N_("git worktree prune [<options>]"),
	NULL
};

static int show_only;
static int verbose;
static unsigned long expire;

static int prune_worktree(const char *id, struct strbuf *reason)
{
	struct stat st;
	char *path;
	int fd, len;

	if (!is_directory(git_path("worktrees/%s", id))) {
		strbuf_addf(reason, _("Removing worktrees/%s: not a valid directory"), id);
		return 1;
	}
	if (file_exists(git_path("worktrees/%s/locked", id)))
		return 0;
	if (stat(git_path("worktrees/%s/gitdir", id), &st)) {
		strbuf_addf(reason, _("Removing worktrees/%s: gitdir file does not exist"), id);
		return 1;
	}
	fd = open(git_path("worktrees/%s/gitdir", id), O_RDONLY);
	if (fd < 0) {
		strbuf_addf(reason, _("Removing worktrees/%s: unable to read gitdir file (%s)"),
			    id, strerror(errno));
		return 1;
	}
	len = st.st_size;
	path = xmalloc(len + 1);
	read_in_full(fd, path, len);
	close(fd);
	while (len && (path[len - 1] == '\n' || path[len - 1] == '\r'))
		len--;
	if (!len) {
		strbuf_addf(reason, _("Removing worktrees/%s: invalid gitdir file"), id);
		free(path);
		return 1;
	}
	path[len] = '\0';
	if (!file_exists(path)) {
		struct stat st_link;
		free(path);
		/*
		 * the repo is moved manually and has not been
		 * accessed since?
		 */
		if (!stat(git_path("worktrees/%s/link", id), &st_link) &&
		    st_link.st_nlink > 1)
			return 0;
		if (st.st_mtime <= expire) {
			strbuf_addf(reason, _("Removing worktrees/%s: gitdir file points to non-existent location"), id);
			return 1;
		} else {
			return 0;
		}
	}
	free(path);
	return 0;
}

static void prune_worktrees(void)
{
	struct strbuf reason = STRBUF_INIT;
	struct strbuf path = STRBUF_INIT;
	DIR *dir = opendir(git_path("worktrees"));
	struct dirent *d;
	int ret;
	if (!dir)
		return;
	while ((d = readdir(dir)) != NULL) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;
		strbuf_reset(&reason);
		if (!prune_worktree(d->d_name, &reason))
			continue;
		if (show_only || verbose)
			printf("%s\n", reason.buf);
		if (show_only)
			continue;
		strbuf_reset(&path);
		strbuf_addstr(&path, git_path("worktrees/%s", d->d_name));
		ret = remove_dir_recursively(&path, 0);
		if (ret < 0 && errno == ENOTDIR)
			ret = unlink(path.buf);
		if (ret)
			error(_("failed to remove: %s"), strerror(errno));
	}
	closedir(dir);
	if (!show_only)
		rmdir(git_path("worktrees"));
	strbuf_release(&reason);
	strbuf_release(&path);
}

static int prune(int ac, const char **av, const char *prefix)
{
	struct option options[] = {
		OPT__DRY_RUN(&show_only, N_("do not remove, show only")),
		OPT__VERBOSE(&verbose, N_("report pruned objects")),
		OPT_EXPIRY_DATE(0, "expire", &expire,
				N_("expire objects older than <time>")),
		OPT_END()
	};

	expire = ULONG_MAX;
	ac = parse_options(ac, av, prefix, options, worktree_usage, 0);
	if (ac)
		usage_with_options(worktree_usage, options);
	prune_worktrees();
	return 0;
}

static char *junk_work_tree;
static char *junk_git_dir;
static int is_junk;
static pid_t junk_pid;

static void remove_junk(void)
{
	struct strbuf sb = STRBUF_INIT;
	if (!is_junk || getpid() != junk_pid)
		return;
	if (junk_git_dir) {
		strbuf_addstr(&sb, junk_git_dir);
		remove_dir_recursively(&sb, 0);
		strbuf_reset(&sb);
	}
	if (junk_work_tree) {
		strbuf_addstr(&sb, junk_work_tree);
		remove_dir_recursively(&sb, 0);
	}
	strbuf_release(&sb);
}

static void remove_junk_on_signal(int signo)
{
	remove_junk();
	sigchain_pop(signo);
	raise(signo);
}

static int add_worktree(const char *path, int force, const char **av)
{
	struct strbuf sb_git = STRBUF_INIT, sb_repo = STRBUF_INIT;
	struct strbuf sb = STRBUF_INIT;
	const char *name;
	struct stat st;
	struct child_process cp;
	int counter = 0, len, ret;
	unsigned char rev[20];

	if (file_exists(path) && !is_empty_dir(path))
		die(_("'%s' already exists"), path);

	len = strlen(path);
	while (len && is_dir_sep(path[len - 1]))
		len--;

	for (name = path + len - 1; name > path; name--)
		if (is_dir_sep(*name)) {
			name++;
			break;
		}
	strbuf_addstr(&sb_repo,
		      git_path("worktrees/%.*s", (int)(path + len - name), name));
	len = sb_repo.len;
	if (safe_create_leading_directories_const(sb_repo.buf))
		die_errno(_("could not create leading directories of '%s'"),
			  sb_repo.buf);
	while (!stat(sb_repo.buf, &st)) {
		counter++;
		strbuf_setlen(&sb_repo, len);
		strbuf_addf(&sb_repo, "%d", counter);
	}
	name = strrchr(sb_repo.buf, '/') + 1;

	junk_pid = getpid();
	atexit(remove_junk);
	sigchain_push_common(remove_junk_on_signal);

	if (mkdir(sb_repo.buf, 0777))
		die_errno(_("could not create directory of '%s'"), sb_repo.buf);
	junk_git_dir = xstrdup(sb_repo.buf);
	is_junk = 1;

	/*
	 * lock the incomplete repo so prune won't delete it, unlock
	 * after the preparation is over.
	 */
	strbuf_addf(&sb, "%s/locked", sb_repo.buf);
	write_file(sb.buf, 1, "initializing\n");

	strbuf_addf(&sb_git, "%s/.git", path);
	if (safe_create_leading_directories_const(sb_git.buf))
		die_errno(_("could not create leading directories of '%s'"),
			  sb_git.buf);
	junk_work_tree = xstrdup(path);

	strbuf_reset(&sb);
	strbuf_addf(&sb, "%s/gitdir", sb_repo.buf);
	write_file(sb.buf, 1, "%s\n", real_path(sb_git.buf));
	write_file(sb_git.buf, 1, "gitdir: %s/worktrees/%s\n",
		   real_path(get_git_common_dir()), name);
	/*
	 * This is to keep resolve_ref() happy. We need a valid HEAD
	 * or is_git_directory() will reject the directory. Any valid
	 * value would do because this value will be ignored and
	 * replaced at the next (real) checkout.
	 */
	if (!resolve_ref_unsafe("HEAD", 0, rev, NULL))
		die(_("unable to resolve HEAD"));
	strbuf_reset(&sb);
	strbuf_addf(&sb, "%s/HEAD", sb_repo.buf);
	write_file(sb.buf, 1, "%s\n", sha1_to_hex(rev));
	strbuf_reset(&sb);
	strbuf_addf(&sb, "%s/commondir", sb_repo.buf);
	write_file(sb.buf, 1, "../..\n");

	fprintf_ln(stderr, _("Enter %s (identifier %s)"), path, name);

	setenv("GIT_CHECKOUT_NEW_WORKTREE", "1", 1);
	setenv(GIT_DIR_ENVIRONMENT, sb_git.buf, 1);
	setenv(GIT_WORK_TREE_ENVIRONMENT, path, 1);
	memset(&cp, 0, sizeof(cp));
	cp.git_cmd = 1;
	argv_array_push(&cp.args, "checkout");
	if (force)
		argv_array_push(&cp.args, "--ignore-other-worktrees");
	for (; *av; av++)
		argv_array_push(&cp.args, *av);
	ret = run_command(&cp);
	if (!ret) {
		is_junk = 0;
		free(junk_work_tree);
		free(junk_git_dir);
		junk_work_tree = NULL;
		junk_git_dir = NULL;
	}
	strbuf_reset(&sb);
	strbuf_addf(&sb, "%s/locked", sb_repo.buf);
	unlink_or_warn(sb.buf);
	strbuf_release(&sb);
	strbuf_release(&sb_repo);
	strbuf_release(&sb_git);
	return ret;
}

static int add(int ac, const char **av, const char *prefix)
{
	int force = 0;
	const char *path;
	struct option options[] = {
		OPT__FORCE(&force, N_("checkout <branch> even if already checked out in other worktree")),
		OPT_END()
	};

	ac = parse_options(ac, av, prefix, options, worktree_usage,
			   PARSE_OPT_STOP_AT_NON_OPTION);
	if (ac < 2)
		usage_with_options(worktree_usage, options);
	path = prefix ? prefix_filename(prefix, strlen(prefix), av[0]) : av[0];
	return add_worktree(path, force, av + 1);
}

int cmd_worktree(int ac, const char **av, const char *prefix)
{
	struct option options[] = {
		OPT_END()
	};

	if (ac < 2)
		usage_with_options(worktree_usage, options);
	if (!strcmp(av[1], "add"))
		return add(ac - 1, av + 1, prefix);
	if (!strcmp(av[1], "prune"))
		return prune(ac - 1, av + 1, prefix);
	usage_with_options(worktree_usage, options);
}

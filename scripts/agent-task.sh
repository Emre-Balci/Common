#!/usr/bin/env bash
# Runs a coding agent on a task in the task's own git worktree, so tasks run in parallel and move
# between agents. The task's state is in its branch `task/<name>` and its handoff file
# `.agents/handoffs/<name>.md`, not in any agent's session (see AGENTS.md).
#
#   scripts/agent-task.sh [-p] <agent> <task> [instructions...]   start or continue a task
#   scripts/agent-task.sh list                                    list task worktrees
#   scripts/agent-task.sh remove <task>                           remove a task's worktree (keeps the branch)
#
# Agents: claude, opencode, codex, hermes. -p runs the agent once without its interactive UI and
# returns when it is done (claude -p, opencode run, codex exec, hermes chat --oneshot), for scripts
# and for one agent delegating to another.
#
# Environment:
#   AGENT_WORKTREES  where worktrees go (default ~/agent-worktrees, on the Linux file system, because
#                    building on /mnt/<drive> is slow). Windows git does not see these paths:
#                    do not run `git worktree prune` from Windows while tasks are open.
#   AGENT_BASE       commit a new task branch starts from (default: HEAD of the main worktree)
set -euo pipefail

usage() {
    sed -n '2,/^set -e/p' "$0" | grep '^#' | sed 's/^# \{0,1\}//' >&2
    exit 2
}

die() {
    printf 'agent-task: %s\n' "$*" >&2
    exit 1
}

common_dir="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" ||
    die "not inside a git repository"
main_root="$(dirname "$common_dir")"
repo_name="$(basename "$main_root")"
worktree_root="${AGENT_WORKTREES:-$HOME/agent-worktrees}/$repo_name"

check_task() {
    [[ "$1" =~ ^[a-z0-9][a-z0-9-]{0,47}$ ]] ||
        die "task name '$1' must be lower-case letters, digits and '-' (at most 48)"
}

cmd="${1:-}"
case "$cmd" in
    "" | -h | --help)
        usage
        ;;
    list)
        git -C "$main_root" worktree list | grep -F '[task/' || echo "no task worktrees"
        exit 0
        ;;
    remove)
        [[ $# -eq 2 ]] || usage
        check_task "$2"
        # Refuses a worktree with uncommitted changes: commit or discard them first.
        git -C "$main_root" worktree remove "$worktree_root/$2"
        echo "removed the worktree; branch task/$2 is kept (git branch -d task/$2 once it is merged)"
        exit 0
        ;;
esac

once=false
if [[ "$cmd" == "-p" ]]; then
    once=true
    shift
fi
[[ $# -ge 2 ]] || usage
agent="$1"
task="$2"
shift 2
instructions="$*"
check_task "$task"

case "$agent" in
    claude | opencode | codex | hermes) ;;
    *) die "unknown agent '$agent' (claude, opencode, codex, hermes)" ;;
esac
command -v "$agent" >/dev/null || die "'$agent' is not on PATH"

dir="$worktree_root/$task"
branch="task/$task"
handoff=".agents/handoffs/$task.md"
if [[ ! -f "$dir/$handoff" && -z "$instructions" ]]; then
    die "new task '$task': give instructions (they become its goal)"
fi
if [[ ! -d "$dir" ]]; then
    mkdir -p "$worktree_root"
    if git -C "$main_root" show-ref --verify --quiet "refs/heads/$branch"; then
        git -C "$main_root" worktree add "$dir" "$branch"
    else
        git -C "$main_root" worktree add -b "$branch" "$dir" "${AGENT_BASE:-HEAD}"
    fi
fi

if [[ ! -f "$dir/$handoff" ]]; then
    template="$dir/.agents/handoff-template.md"
    [[ -f "$template" ]] || die "$template is missing: start the task from a branch that has it"
    {
        printf '# Handoff: %s\n\n## Goal\n%s\n\n' "$task" "$instructions"
        printf '## State (%s, created by agent-task.sh)\n- Branch `%s`; nothing done yet.\n\n' \
            "$(date +%F)" "$branch"
        printf '## Findings\n\n## Not verified\n\n## Next steps\n1. Start on the goal.\n'
    } >"$dir/$handoff"
fi

prompt="You are working on task '$task' in a git worktree on branch $branch. First read AGENTS.md \
and $handoff, which holds the goal and the state the previous agent left. \
${instructions:+Instructions for this session: $instructions } \
Before you stop, update $handoff (State, Not verified, Next steps) so another agent can continue, \
and commit your work and the handoff on $branch. Do not push or merge."

cd "$dir"
echo "agent-task: $agent on $branch in $dir" >&2
if [[ "$once" == true ]]; then
    case "$agent" in
        claude) exec claude -p "$prompt" ;;
        opencode) exec opencode run "$prompt" ;;
        codex) exec codex exec "$prompt" ;;
        hermes) exec hermes chat --oneshot -q "$prompt" ;;
    esac
else
    case "$agent" in
        claude) exec claude "$prompt" ;;
        opencode) exec opencode --prompt "$prompt" . ;;
        codex) exec codex "$prompt" ;;
        hermes) exec hermes chat -q "$prompt" ;;
    esac
fi

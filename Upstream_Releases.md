# Moving Waterpoint onto a new upstream release

Waterpoint's source is upstream LambdaMOO
([wrog/lambdamoo](https://github.com/wrog/lambdamoo)) plus our own
changes.  The `waterpoint` branch is always an upstream release tag
with our changes on top as a linear series of commits, one per
logical change.  When upstream makes a new release, we build a fresh
series on the new tag and swap it in, rather than merging the release
into the old branch.  Between upstream releases the series only grows:
new changes go on top as ordinary commits, and history is rewritten
only when moving to a new tag.

## The branch

- The base is an upstream release tag (`v1.9.0alpha` today), never
  upstream's `dev` branch.
- On top of it is a linear series of our commits: no merge commits, no
  review-fixup commits, no "adopt upstream's version" follow-ups.  Each
  commit builds.
- Our GitHub workflows (`.github/`) and the README note at the top of
  `README.md` are part of the series and get carried onto every new
  base like any other change of ours.
- A change upstream has taken in a different form is carried as
  upstream's own commit (cherry-picked with its author and message) in
  place of ours, until a release includes it.
- Every commit's author and committer is a person.  Tools are never
  authors or co-authors.

## Procedure for a new upstream release

1. Fetch upstream and its tags:

       git remote add upstream https://github.com/wrog/lambdamoo   # once
       git fetch upstream --tags

2. List our series and decide each commit's fate against the new tag
   (`NEW` below):

       git log --reverse --format='%h %an %s' OLD_TAG..waterpoint

   - Already in the release, as ours or upstream's revision of it: drop.
   - Upstream took it in a different form on `dev` but hasn't released
     it: replace ours with a cherry-pick of upstream's commit.
   - Upstream took part of it: carry upstream's commit and keep only
     what is still ours as a separate commit on top.  Our PCRE2
     migration is due for this once a release includes upstream's
     PCRE2 support (e8bdf09).
   - Still ours alone: keep.  Fold any later fixups into it.

   `git cherry NEW waterpoint` marks commits whose patch is
   already upstream with `-`, but a revised version won't match, so
   check the release notes and `git log upstream/dev` by subject too.

3. Build the new series on a work branch:

       git checkout -b waterpoint-next NEW
       git cherry-pick <kept commits, in order>

   Resolve conflicts commit by commit, keeping each one a single
   change.  Build at every commit:

       git rebase --exec 'autoconf && ./configure && make -j8' NEW

4. Check the result.  The diff between the old and new branches should
   come only from the upstream release, never from a change of ours
   that went missing:

       git diff waterpoint waterpoint-next      # review
       git range-diff OLD_TAG..waterpoint NEW..waterpoint-next

   Run the lmt suite against a build of `waterpoint-next`, and push it
   so CI runs.

5. Swap the branches.  Keep the old branch under an archive name,
   `waterpoint-<old base>-wp-<last N>` (e.g. `waterpoint-1.9.0alpha-wp-5`),
   naming the upstream base and the last Waterpoint release built on it:

       git push origin waterpoint:refs/heads/waterpoint-1.9.0alpha-wp-5
       git push --force-with-lease=waterpoint:<old tip> \
           origin waterpoint-next:waterpoint

   The repository's rules block force pushes to `waterpoint`, so a
   person with admin rights does the second push (or relaxes the rule
   for it).  Open PRs keep targeting `waterpoint`; rebase each one's
   branch onto the new series (below).  The archived branch stays as the
   record of what ran on the old base; don't delete it.

6. Tag the next Waterpoint release (`wp-N`) on the new branch when it
   ships.  Older `wp-N` tags stay on the archived branch, so until the
   first release on the new base `server_version()` reports no `+wp.N`
   (see `Version_Source.md`).

## Upstream fixes carried ahead of a release

Fixes land on upstream's `dev` branch some time before they appear in a
release.  When one matters to us we take it early rather than wait, as
upstream's own commit (author and message intact) so that step 2 above
drops it wholesale once a release carries it.  These are the ones on the
branch now, in series order:

| commit | what |
|---|---|
| `1419569` | `log_command_history()` dereferenced a null `command_history` inside `panic()` |
| `d1222f7` | `NumNum`/`UNumNum` were 64-bit in the INT16 build |
| `02fe437` | emergency mode's bootstrap wizard got a garbage `.owner` |
| `d6cbddf` | emergency `quit`/`abort` skipped `network_shutdown()`, orphaning the `NET_LOCAL` socket |
| `7bbf288` | non-Unicode `get_utf()` sign-extended bytes >= 0x80, so a trailing 0xFF read as `EOF` |
| `d167aca`, `df39f4f`, `c741e3c` | prerequisites of `8e9596e`, not wanted for themselves |
| `8e9596e` | `call_function()` leaked continuation state on tail calls and on killed suspended tasks |
| `698e275` | `min()`/`max()` leaked a reference on type mismatch in boxed-float builds |
| `31a3c72` | a dump whose `fflush`/`fsync`/`fclose` failed was renamed over the live database |
| `b1829b1` | `isspace()`/`isdigit()` on plain `char` in `db_io.c` and `db_verbs.c` |
| `fbd7e9e` | the `net_single.c` half only; the `net_multi.c` half is ours, in "Fix ways a task could hang the whole server" |
| `7d9d129` | `CodingStyle.md` |

### Deliberately not carried

Reconsider each at the next release rather than before it:

- The build-system series on `dev` (`ax_lp`, `ax_xt`, `ax_subst_pp`,
  `f3ceb7d`'s gnulib `AM_ICONV` macros, `68eb5c4`, `305a96d`).  No
  runtime effect, and it arrives with the release.
- Upstream's unit-test harness (`92a5124`, `bbd8f96`, `4ca16d2`) and the
  three fixes that are only visible under it (`da547f8`, `7179e8e`,
  `9f29bb6`).  Worth revisiting once a release includes the harness.
- `c6698ca` (gperf 3.0 compat), `256fbf6`, `da83c32`, `8c26827`,
  `4a3e0f5`: portability we don't currently need.
- `5c50680` (split `pcre.h`/`pcre2.h` configure checks) and `9e074b4`
  (`pattern_*` style), both of which touch code our PCRE2 commits
  rewrote.
- `468292a` (empty initial database file), `53b5724` (`regexp` group in
  `server_version()`), `1181aa8` (`myrealloc()` in `grew()`),
  `f646a04` (`str(r)index()` returning `Num`): features and hygiene, not
  fixes.

### Divergences to settle at the next release

Both of these fix a bug we had already fixed differently, so they will
conflict and one approach has to be chosen:

- `e2553da` vs our "Fix server panic on ranges that end at the largest
  integer".  Upstream shifts the whole from-after window, treating
  `after == NUM_MIN` as `NUM_MAX+1`, across `execute.c`, `list.c`,
  `list.h` and `utf.[ch]`, and documents the API; we clamp with
  `range_after()`.  Theirs is the more thorough treatment; expect to drop
  ours for it.
- `9bc5e2e` vs our "Skip unmatched groups in substitute()".  Ours skips
  `start == 0`, the `{0, -1}` unmatched group that was the actual bug;
  theirs skips `start > end`, which also short-circuits an empty match.
  Same defect covered either way.

Also note that `bae10bf` makes ylo-plus-Unicode a `configure` error,
which meets our "Use PCRE2 for match() and rmatch() in every build".

## Open pull requests

For a PR whose branch was based on commit `B` of the old series, find
the commit in the new series that corresponds to `B` (same subject;
`git range-diff` pairs them) and move the PR's own commits onto it:

    git rebase --onto <new B> B <pr-branch>
    git push --force-with-lease origin <pr-branch>

Then change the PR's base back to `waterpoint`.  If `B` was dropped or
replaced by upstream's commit, use the nearest commit after it.

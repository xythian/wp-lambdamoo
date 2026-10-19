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

## Open pull requests

For a PR whose branch was based on commit `B` of the old series, find
the commit in the new series that corresponds to `B` (same subject;
`git range-diff` pairs them) and move the PR's own commits onto it:

    git rebase --onto <new B> B <pr-branch>
    git push --force-with-lease origin <pr-branch>

Then change the PR's base back to `waterpoint`.  If `B` was dropped or
replaced by upstream's commit, use the nearest commit after it.

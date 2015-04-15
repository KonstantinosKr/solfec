# hg tips and tricks #

This page assumes you are happy using hg to get a local copy of the repo, pull the latest updates, make changes and push them back to the googlecode repo. It describes a couple of ways to make handling changes neater.

It refers to extensions - for how to install these see [here](http://mercurial.selenic.com/wiki/UsingExtensions), noting that on Windows your "hgrc" file is actually an .ini file. The locations this hgrc file can be in are described [here](http://www.selenic.com/mercurial/hgrc.5.html).

# collapse #
When working on a feature you want (should?) to commit regularly to your local repo. The `Collapse` extension can be useful so that:

> "Rather than filling the changelog with changes of the "OK it almost   works now" type, you can collapse all of your intermediate commits before pushing to the shared repository."

For where to get this extension see the [Collapse mercurial wiki page](http://mercurial.selenic.com/wiki/CollapseExtension).

# rebase #
When someone's modified solfec between you getting the latest changes, and you being ready to push your changes, you end up having to pull again and merge locally before pushing. Sometimes you might prefer that the history shows your changes as coming after of the other person's changes, rather than as parallel development.

The [Rebase extension](http://mercurial.selenic.com/wiki/RebaseExtension) lets you do this, by moving your changes to be made against a different changeset. _However_ someone on stackoverflow had a persuasave argument that it is usually better to see in the history what the code state was the that changes were actually made against, and what changes (if any) were subsequently necessary to merge them into the latest code, so use with discretion.
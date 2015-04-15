# Why Does hg push Fail? #
If you do `hg push`, then enter your password, and you get:

`abort: HTTP Error 403: Forbidden`

Its probably that you're entering the wrong password.  It's not the one you log in to google with, but the one shown (if you are logged in) at
http://code.google.com/p/solfec/source/checkout

You can save a default password to avoid entering this every time - see instructions [here](http://www.selenic.com/mercurial/wiki/FAQ#FAQ.2FCommonProblems.How_can_I_store_my_HTTP_login_once_and_for_all_.3F) but note you should make sure your hgrc file isn't readable by others if you do this.
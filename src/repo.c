/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo.c
 *
 * Manage metadata coming from one repository
 *
 */

#define _GNU_SOURCE
#include <string.h>
#include <fnmatch.h>

#include <stdio.h>
#include <stdlib.h>



#include "repo.h"
#include "pool.h"
#include "poolid_private.h"
#include "util.h"
#include "chksum.h"

#define IDARRAY_BLOCK     4095


/*
 * create empty repo
 * and add to pool
 */

Repo *
repo_create(Pool *pool, const char *name)
{
  Repo *repo;

  pool_freewhatprovides(pool);
  repo = (Repo *)solv_calloc(1, sizeof(*repo));
  if (!pool->nrepos)
    {
      pool->nrepos = 1;	/* start with repoid 1 */
      pool->repos = (Repo **)solv_calloc(2, sizeof(Repo *));
    }
  else
    pool->repos = (Repo **)solv_realloc2(pool->repos, pool->nrepos + 1, sizeof(Repo *));
  pool->repos[pool->nrepos] = repo;
  pool->urepos++;
  repo->repoid = pool->nrepos++;
  repo->name = name ? solv_strdup(name) : 0;
  repo->pool = pool;
  repo->start = pool->nsolvables;
  repo->end = pool->nsolvables;
  repo->nsolvables = 0;
  return repo;
}

void
repo_freedata(Repo *repo)
{
  int i;
  for (i = 1; i < repo->nrepodata; i++)
    repodata_freedata(repo->repodata + i);
  solv_free(repo->repodata);
  solv_free(repo->idarraydata);
  solv_free(repo->rpmdbid);
  solv_free(repo->lastidhash);
  solv_free((char *)repo->name);
  solv_free(repo);
}

/* delete all solvables and repodata blocks from this repo */

void
repo_empty(Repo *repo, int reuseids)
{
  Pool *pool = repo->pool;
  Solvable *s;
  int i;

  pool_freewhatprovides(pool);
  if (reuseids && repo->end == pool->nsolvables)
    {
      /* it's ok to reuse the ids. As this is the last repo, we can
         just shrink the solvable array */
      for (i = repo->end - 1, s = pool->solvables + i; i >= repo->start; i--, s--)
	if (s->repo != repo)
	  break;
      pool_free_solvable_block(pool, i + 1, repo->end - (i + 1), reuseids);
      repo->end = i + 1;
    }
  /* zero out (i.e. free) solvables belonging to this repo */
  for (i = repo->start, s = pool->solvables + i; i < repo->end; i++, s++)
    if (s->repo == repo)
      memset(s, 0, sizeof(*s));
  repo->end = repo->start;
  repo->nsolvables = 0;

  /* free all data belonging to this repo */
  repo->idarraydata = solv_free(repo->idarraydata);
  repo->idarraysize = 0;
  repo->lastoff = 0;
  repo->rpmdbid = solv_free(repo->rpmdbid);
  for (i = 1; i < repo->nrepodata; i++)
    repodata_freedata(repo->repodata + i);
  solv_free(repo->repodata);
  repo->repodata = 0;
  repo->nrepodata = 0;
}

/*
 * remove repo from pool, delete solvables
 *
 */

void
repo_free(Repo *repo, int reuseids)
{
  Pool *pool = repo->pool;
  int i;

  if (repo == pool->installed)
    pool->installed = 0;
  repo_empty(repo, reuseids);
  for (i = 1; i < pool->nrepos; i++)	/* find repo in pool */
    if (pool->repos[i] == repo)
      break;
  if (i == pool->nrepos)	       /* repo not in pool, return */
    return;
  if (i == pool->nrepos - 1 && reuseids)
    pool->nrepos--;
  else
    pool->repos[i] = 0;
  pool->urepos--;
  repo_freedata(repo);
}

Id
repo_add_solvable(Repo *repo)
{
  Id p = pool_add_solvable(repo->pool);
  if (!repo->start || repo->start == repo->end)
    repo->start = repo->end = p;
  /* warning: sidedata must be extended before adapting start/end */
  if (repo->rpmdbid)
    repo->rpmdbid = (Id *)repo_sidedata_extend(repo, repo->rpmdbid, sizeof(Id), p, 1);
  if (p < repo->start)
    repo->start = p;
  if (p + 1 > repo->end)
    repo->end = p + 1;
  repo->nsolvables++;
  repo->pool->solvables[p].repo = repo;
  return p;
}

Id
repo_add_solvable_block(Repo *repo, int count)
{
  Id p;
  Solvable *s;
  if (!count)
    return 0;
  p = pool_add_solvable_block(repo->pool, count);
  if (!repo->start || repo->start == repo->end)
    repo->start = repo->end = p;
  /* warning: sidedata must be extended before adapting start/end */
  if (repo->rpmdbid)
    repo->rpmdbid = (Id *)repo_sidedata_extend(repo, repo->rpmdbid, sizeof(Id), p, count);
  if (p < repo->start)
    repo->start = p;
  if (p + count > repo->end)
    repo->end = p + count;
  repo->nsolvables += count;
  for (s = repo->pool->solvables + p; count--; s++)
    s->repo = repo;
  return p;
}

void
repo_free_solvable(Repo *repo, Id p, int reuseids)
{
  repo_free_solvable_block(repo, p, 1, reuseids);
}

void
repo_free_solvable_block(Repo *repo, Id start, int count, int reuseids)
{
  Solvable *s;
  Repodata *data;
  int i;
  if (start + count == repo->end)
    repo->end -= count;
  repo->nsolvables -= count;
  for (s = repo->pool->solvables + start, i = count; i--; s++)
    s->repo = 0;
  pool_free_solvable_block(repo->pool, start, count, reuseids);
  FOR_REPODATAS(repo, i, data)
    {
      int dstart, dend;
      if (data->end > repo->end)
        repodata_shrink(data, repo->end);
      dstart = data->start > start ? data->start : start;
      dend = data->end < start + count ? data->end : start + count;
      if (dstart < dend)
	{
	  if (data->attrs)
	    {
	      int j;
	      for (j = dstart; j < dend; j++)	
		data->attrs[j - data->start] = solv_free(data->attrs[j - data->start]);
	      if (data->lasthandle >= dstart && data->lasthandle < dend)
	        data->lasthandle = 0;
	    }
	  if (data->incoreoffset)
	    memset(data->incoreoffset + (dstart - data->start), 0, (dend - dstart) * sizeof(Id));
	}
    }
}

/* specialized version of repo_add_solvable_block that inserts the new solvable
 * block before the indicated repo, which gets relocated.
 * used in repo_add_rpmdb
 */
Id
repo_add_solvable_block_before(Repo *repo, int count, Repo *beforerepo)
{
  Pool *pool = repo->pool;
  Id p;
  Solvable *s;
  Repodata *data;
  int i;

  if (!count || !beforerepo || beforerepo->end != pool->nsolvables || beforerepo->start == beforerepo->end)
    return repo_add_solvable_block(repo, count);
  p = beforerepo->start;
  /* make sure all solvables belong to beforerepo */
  for (i = p, s = pool->solvables + i; i < beforerepo->end; i++, s++)
    if (s->repo && s->repo != beforerepo)
      return repo_add_solvable_block(repo, count);
  /* now move beforerepo to back */
  pool_add_solvable_block(pool, count);	/* must return beforerepo->end! */
  memmove(pool->solvables + p + count, pool->solvables + p, (beforerepo->end - p) * sizeof(Solvable));
  memset(pool->solvables + p, 0, sizeof(Solvable) * count);
  /* adapt repodata */
  FOR_REPODATAS(beforerepo, i, data)
    {
      if (data->start < p)
	continue;
      data->start += count;
      data->end += count;
    }
  beforerepo->start += count;
  beforerepo->end += count;
  /* we now have count free solvables at id p */
  /* warning: sidedata must be extended before adapting start/end */
  if (repo->rpmdbid)
    repo->rpmdbid = (Id *)repo_sidedata_extend(repo, repo->rpmdbid, sizeof(Id), p, count);
  if (p < repo->start)
    repo->start = p;
  if (p + count > repo->end)
    repo->end = p + count;
  repo->nsolvables += count;
  for (s = pool->solvables + p; count--; s++)
    s->repo = repo;
  return p;
}


/* repository sidedata is solvable data allocated on demand.
 * It is used for data that is normally not present
 * in the solvable like the rpmdbid.
 * The solvable allocation funcions need to make sure that
 * the sidedata gets extended if new solvables get added.
 */

#define REPO_SIDEDATA_BLOCK 63

void *
repo_sidedata_create(Repo *repo, size_t size)
{
  return solv_calloc_block(repo->end - repo->start, size, REPO_SIDEDATA_BLOCK);
}

void *
repo_sidedata_extend(Repo *repo, void *b, size_t size, Id p, int count)
{
  int n = repo->end - repo->start;
  if (p < repo->start)
    {
      int d = repo->start - p;
      b = solv_extend(b, n, d, size, REPO_SIDEDATA_BLOCK);
      memmove((char *)b + d * size, b, n * size);
      memset(b, 0, d * size);
      n += d;
    }
  if (p + count > repo->end)
    {
      int d = p + count - repo->end;
      b = solv_extend(b, n, d, size, REPO_SIDEDATA_BLOCK);
      memset((char *)b + n * size, 0, d * size);
    }
  return b;
}

/*
 * add Id to idarraydata used to store dependencies
 * olddeps: old array offset to extend
 * returns new array offset
 */

Offset
repo_addid(Repo *repo, Offset olddeps, Id id)
{
  Id *idarray;
  int idarraysize;
  int i;

  idarray = repo->idarraydata;
  idarraysize = repo->idarraysize;

  if (!idarray)			       /* alloc idarray if not done yet */
    {
      idarraysize = 1;
      idarray = solv_extend_resize(0, 1, sizeof(Id), IDARRAY_BLOCK);
      idarray[0] = 0;
      repo->lastoff = 0;
    }

  if (!olddeps)				/* no deps yet */
    {
      olddeps = idarraysize;
      idarray = solv_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
    }
  else if (olddeps == repo->lastoff)	/* extend at end */
    idarraysize--;
  else					/* can't extend, copy old */
    {
      i = olddeps;
      olddeps = idarraysize;
      for (; idarray[i]; i++)
        {
	  idarray = solv_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
          idarray[idarraysize++] = idarray[i];
        }
      idarray = solv_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
    }

  idarray[idarraysize++] = id;		/* insert Id into array */
  idarray = solv_extend(idarray, idarraysize, 1, sizeof(Id), IDARRAY_BLOCK);
  idarray[idarraysize++] = 0;		/* ensure NULL termination */

  repo->idarraydata = idarray;
  repo->idarraysize = idarraysize;
  repo->lastoff = olddeps;

  return olddeps;
}

#define REPO_ADDID_DEP_HASHTHRES	64
#define REPO_ADDID_DEP_HASHMIN		128

/*
 * Optimization for packages with an excessive amount of provides/requires:
 * if the number of deps exceed a threshold, we build a hash of the already
 * seen ids.
 */
static Offset
repo_addid_dep_hash(Repo *repo, Offset olddeps, Id id, Id marker, int size)
{
  Id oid, *oidp;
  int before;
  Hashval h, hh;
  Id hid;

  before = 0;
  if (marker)
    {
      if (marker < 0)
	{
	  marker = -marker;
	  before = 1;
	}
      if (marker == id)
	marker = 0;
    }

  /* maintain hash and lastmarkerpos */
  if (repo->lastidhash_idarraysize != repo->idarraysize || (Hashval)size * 2 > repo->lastidhash_mask || repo->lastmarker != marker)
    {
      repo->lastmarkerpos = 0;
      if ((Hashval)size * 2 > repo->lastidhash_mask)
	{
	  repo->lastidhash_mask = mkmask(size < REPO_ADDID_DEP_HASHMIN ? REPO_ADDID_DEP_HASHMIN : size);
	  repo->lastidhash = solv_realloc2(repo->lastidhash, repo->lastidhash_mask + 1, sizeof(Id));
	}
      memset(repo->lastidhash, 0, (repo->lastidhash_mask + 1) * sizeof(Id));
      for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != 0; oidp++)
	{
	  h = oid & repo->lastidhash_mask;
	  hh = HASHCHAIN_START;
	  while (repo->lastidhash[h] != 0)
	    h = HASHCHAIN_NEXT(h, hh, repo->lastidhash_mask);
	  repo->lastidhash[h] = oid;
	  if (marker && oid == marker)
	    repo->lastmarkerpos = oidp - repo->idarraydata;
	}
      repo->lastmarker = marker;
      repo->lastidhash_idarraysize = repo->idarraysize;
    }

  /* check the hash! */
  h = id & repo->lastidhash_mask;
  hh = HASHCHAIN_START;
  while ((hid = repo->lastidhash[h]) != 0 && hid != id)
    h = HASHCHAIN_NEXT(h, hh, repo->lastidhash_mask);
  /* put new element in hash */
  if (!hid)
    repo->lastidhash[h] = id;
  else if (marker == SOLVABLE_FILEMARKER && (!before || !repo->lastmarkerpos))
    return olddeps;
  if (marker && !before && !repo->lastmarkerpos)
    {
      /* we have to add the marker first */
      repo->lastmarkerpos = repo->idarraysize - 1;
      olddeps = repo_addid(repo, olddeps, marker);
      /* now put marker in hash */
      h = marker & repo->lastidhash_mask;
      hh = HASHCHAIN_START;
      while (repo->lastidhash[h] != 0)
	h = HASHCHAIN_NEXT(h, hh, repo->lastidhash_mask);
      repo->lastidhash[h] = marker;
      repo->lastidhash_idarraysize = repo->idarraysize;
    }
  if (!hid)
    {
      /* new entry, insert in correct position */
      if (marker && before && repo->lastmarkerpos)
	{
	  /* need to add it before the marker */
	  olddeps = repo_addid(repo, olddeps, id);	/* dummy to make room */
	  memmove(repo->idarraydata + repo->lastmarkerpos + 1, repo->idarraydata + repo->lastmarkerpos, (repo->idarraysize - repo->lastmarkerpos - 2) * sizeof(Id));
	  repo->idarraydata[repo->lastmarkerpos++] = id;
	}
      else
	{
	  /* just append it to the end */
	  olddeps = repo_addid(repo, olddeps, id);
	}
      repo->lastidhash_idarraysize = repo->idarraysize;
      return olddeps;
    }
  /* we already have it in the hash */
  if (!marker)
    return olddeps;
  if (marker == SOLVABLE_FILEMARKER)
    {
      /* check if it is in the wrong half */
      /* (we already made sure that "before" and "lastmarkerpos" are set, see above) */
      for (oidp = repo->idarraydata + repo->lastmarkerpos + 1; (oid = *oidp) != 0; oidp++)
	if (oid == id)
	  break;
      if (!oid)
	return olddeps;
      /* yes, wrong half. copy it over */
      memmove(repo->idarraydata + repo->lastmarkerpos + 1, repo->idarraydata + repo->lastmarkerpos, (oidp - (repo->idarraydata + repo->lastmarkerpos)) * sizeof(Id));
      repo->idarraydata[repo->lastmarkerpos++] = id;
      return olddeps;
    }
  if (before)
    return olddeps;
  /* check if it is in the correct half */
  for (oidp = repo->idarraydata + repo->lastmarkerpos + 1; (oid = *oidp) != 0; oidp++)
    if (oid == id)
      return olddeps;
  /* nope, copy it over */
  for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != 0; oidp++)
    if (oid == id)
      break;
  if (!oid)
    return olddeps;	/* should not happen */
  memmove(oidp, oidp + 1, (repo->idarraydata + repo->idarraysize - oidp - 2) * sizeof(Id));
  repo->idarraydata[repo->idarraysize - 2] = id;
  repo->lastmarkerpos--;	/* marker has been moved */
  return olddeps;
}

/*
 * add dependency (as Id) to repo, also unifies dependencies
 * olddeps = offset into idarraydata
 * marker= 0 for normal dep
 * marker > 0 add dep after marker
 * marker < 0 add dep before -marker
 * returns new start of dependency array
 */
Offset
repo_addid_dep(Repo *repo, Offset olddeps, Id id, Id marker)
{
  Id oid, *oidp, *markerp;
  int before;

  if (!olddeps)
    {
      if (marker > 0)
	olddeps = repo_addid(repo, olddeps, marker);
      return repo_addid(repo, olddeps, id);
    }

  /* check if we should use the hash optimization */
  if (olddeps == repo->lastoff)
    {
      int size = repo->idarraysize - 1 - repo->lastoff;
      if (size >= REPO_ADDID_DEP_HASHTHRES)
        return repo_addid_dep_hash(repo, olddeps, id, marker, size);
    }

  before = 0;
  if (marker)
    {
      if (marker < 0)
	{
	  marker = -marker;
	  before = 1;
	}
      if (marker == id)
	marker = 0;
    }

  if (!marker)
    {
      for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != 0; oidp++)
        if (oid == id)
	  return olddeps;
      return repo_addid(repo, olddeps, id);
    }

  markerp = 0;
  for (oidp = repo->idarraydata + olddeps; (oid = *oidp) != 0; oidp++)
    {
      if (oid == marker)
	markerp = oidp;
      else if (oid == id)
	break;
    }

  if (oid)
    {
      if (marker == SOLVABLE_FILEMARKER)
	{
	  if (!markerp || !before)
            return olddeps;
          /* we found it, but in the second half */
          memmove(markerp + 1, markerp, (oidp - markerp) * sizeof(Id));
          *markerp = id;
          return olddeps;
	}
      if (markerp || before)
        return olddeps;
      /* we found it, but in the first half */
      markerp = oidp++;
      for (; (oid = *oidp) != 0; oidp++)
        if (oid == marker)
          break;
      if (!oid)
        {
	  /* no marker in array yet */
          oidp--;
          if (markerp < oidp)
            memmove(markerp, markerp + 1, (oidp - markerp) * sizeof(Id));
          *oidp = marker;
          return repo_addid(repo, olddeps, id);
        }
      while (oidp[1])
        oidp++;
      memmove(markerp, markerp + 1, (oidp - markerp) * sizeof(Id));
      *oidp = id;
      return olddeps;
    }
  /* id not yet in array */
  if (!before && !markerp)
    olddeps = repo_addid(repo, olddeps, marker);
  else if (before && markerp)
    {
      *markerp++ = id;
      id = *--oidp;
      if (markerp < oidp)
        memmove(markerp + 1, markerp, (oidp - markerp) * sizeof(Id));
      *markerp = marker;
    }
  return repo_addid(repo, olddeps, id);
}

/* return standard marker for the keyname dependency.
 * 1: return positive marker, -1: return negative marker
 */
Id
solv_depmarker(Id keyname, Id marker)
{
  if (marker != 1 && marker != -1)
    return marker;
  if (keyname == SOLVABLE_PROVIDES)
    return marker < 0 ? -SOLVABLE_FILEMARKER : SOLVABLE_FILEMARKER;
  if (keyname == SOLVABLE_REQUIRES)
    return marker < 0 ? -SOLVABLE_PREREQMARKER : SOLVABLE_PREREQMARKER;
  return 0;
}

/*
 * reserve Ids
 * make space for 'num' more dependencies
 * returns new start of dependency array
 *
 * reserved ids will always begin at offset idarraysize
 */
Offset
repo_reserve_ids(Repo *repo, Offset olddeps, int num)
{
  num++;	/* room for trailing ID_NULL */

  if (!repo->idarraysize)	       /* ensure buffer space */
    {
      repo->idarraysize = 1;
      repo->idarraydata = solv_extend_resize(0, 1 + num, sizeof(Id), IDARRAY_BLOCK);
      repo->idarraydata[0] = 0;
      repo->lastoff = 1;
      return 1;
    }

  if (olddeps && olddeps != repo->lastoff)   /* if not appending */
    {
      /* can't insert into idarray, this would invalidate all 'larger' offsets
       * so create new space at end and move existing deps there.
       * Leaving 'hole' at old position.
       */

      Id *idstart, *idend;
      int count;

      for (idstart = idend = repo->idarraydata + olddeps; *idend++; )   /* find end */
	;
      count = idend - idstart - 1 + num;	       /* new size */

      repo->idarraydata = solv_extend(repo->idarraydata, repo->idarraysize, count, sizeof(Id), IDARRAY_BLOCK);
      /* move old deps to end */
      olddeps = repo->lastoff = repo->idarraysize;
      memcpy(repo->idarraydata + olddeps, idstart, count - num);
      repo->idarraysize = olddeps + count - num;

      return olddeps;
    }

  if (olddeps)			       /* appending */
    repo->idarraysize--;

  /* make room*/
  repo->idarraydata = solv_extend(repo->idarraydata, repo->idarraysize, num, sizeof(Id), IDARRAY_BLOCK);

  /* appending or new */
  repo->lastoff = olddeps ? olddeps : repo->idarraysize;

  return repo->lastoff;
}


/***********************************************************************/

struct matchdata
{
  Pool *pool;
  int flags;
  Datamatcher matcher;
  int stop;
  Id *keyskip;
  int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv);
  void *callback_data;
};

static int
repo_matchvalue(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  struct matchdata *md = cbdata;

  if (md->matcher.match)
    {
      const char *str;
      if (key->name == SOLVABLE_FILELIST && key->type == REPOKEY_TYPE_DIRSTRARRAY && (md->matcher.flags & SEARCH_FILES) != 0)
	if (!datamatcher_checkbasename(&md->matcher, kv->str))
	  return 0;
      if (!(str = repodata_stringify(md->pool, data, key, kv, md->flags)))
	return 0;
      if (!datamatcher_match(&md->matcher, str))
	return 0;
    }
  else
    {
      /* stringify filelist if requested */
      if (key->name == SOLVABLE_FILELIST && key->type == REPOKEY_TYPE_DIRSTRARRAY && (md->matcher.flags & SEARCH_FILES) != 0)
        repodata_stringify(md->pool, data, key, kv, md->flags);
    }
  md->stop = md->callback(md->callback_data, s, data, key, kv);
  return md->stop;
}


/* list of all keys we store in the solvable */
/* also used in the dataiterator code in repodata.c */
Repokey repo_solvablekeys[RPM_RPMDBID - SOLVABLE_NAME + 1] = {
  { SOLVABLE_NAME,        REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ARCH,        REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_EVR,         REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_VENDOR,      REPOKEY_TYPE_ID, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_PROVIDES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_OBSOLETES,   REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_CONFLICTS,   REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_REQUIRES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_RECOMMENDS,  REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUGGESTS,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_SUPPLEMENTS, REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { SOLVABLE_ENHANCES,    REPOKEY_TYPE_IDARRAY, 0, KEY_STORAGE_SOLVABLE },
  { RPM_RPMDBID,          REPOKEY_TYPE_NUM, 0, KEY_STORAGE_SOLVABLE },
};

static void
domatch_idarray(Solvable *s, Id keyname, struct matchdata *md, Id *ida)
{
  KeyValue kv;
  kv.entry = 0;
  kv.parent = 0;
  for (; *ida && !md->stop; ida++)
    {
      kv.id = *ida;
      kv.eof = ida[1] ? 0 : 1;
      repo_matchvalue(md, s, 0, repo_solvablekeys + (keyname - SOLVABLE_NAME), &kv);
      kv.entry++;
    }
}

static Offset *
solvable_offsetptr(Solvable *s, Id keyname)
{
  switch(keyname)
  {
  case SOLVABLE_PROVIDES:
    return &s->provides;
  case SOLVABLE_OBSOLETES:
    return &s->obsoletes;
  case SOLVABLE_CONFLICTS:
    return &s->conflicts;
  case SOLVABLE_REQUIRES:
    return &s->requires;
  case SOLVABLE_RECOMMENDS:
    return &s->recommends;
  case SOLVABLE_SUGGESTS:
    return &s->suggests;
  case SOLVABLE_SUPPLEMENTS:
    return &s->supplements;
  case SOLVABLE_ENHANCES:
    return &s->enhances;
  default:
    return 0;
  }
}

static void
repo_search_md(Repo *repo, Id p, Id keyname, struct matchdata *md)
{
  KeyValue kv;
  Pool *pool = repo->pool;
  Repodata *data;
  int i, flags;
  Solvable *s;
  Id *keyskip;

  kv.parent = 0;
  md->stop = 0;
  if (!p)
    {
      for (p = repo->start, s = repo->pool->solvables + p; p < repo->end; p++, s++)
	{
	  if (s->repo == repo)
            repo_search_md(repo, p, keyname, md);
	  if (md->stop > SEARCH_NEXT_SOLVABLE)
	    break;
	}
      return;
    }
  if (p < 0 && p != SOLVID_META)
    return;		/* SOLVID_POS not supported yet */
  flags = md->flags;
  if (p > 0 && !(flags & SEARCH_NO_STORAGE_SOLVABLE))
    {
      s = pool->solvables + p;
      switch(keyname)
	{
	  case 0:
	  case SOLVABLE_NAME:
	    if (s->name)
	      {
		kv.id = s->name;
		repo_matchvalue(md, s, 0, repo_solvablekeys + 0, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_ARCH:
	    if (s->arch)
	      {
		kv.id = s->arch;
		repo_matchvalue(md, s, 0, repo_solvablekeys + 1, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_EVR:
	    if (s->evr)
	      {
		kv.id = s->evr;
		repo_matchvalue(md, s, 0, repo_solvablekeys + 2, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_VENDOR:
	    if (s->vendor)
	      {
		kv.id = s->vendor;
		repo_matchvalue(md, s, 0, repo_solvablekeys + 3, &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_PROVIDES:
	    if (s->provides)
	      domatch_idarray(s, SOLVABLE_PROVIDES, md, repo->idarraydata + s->provides);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_OBSOLETES:
	    if (s->obsoletes)
	      domatch_idarray(s, SOLVABLE_OBSOLETES, md, repo->idarraydata + s->obsoletes);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_CONFLICTS:
	    if (s->conflicts)
	      domatch_idarray(s, SOLVABLE_CONFLICTS, md, repo->idarraydata + s->conflicts);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_REQUIRES:
	    if (s->requires)
	      domatch_idarray(s, SOLVABLE_REQUIRES, md, repo->idarraydata + s->requires);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_RECOMMENDS:
	    if (s->recommends)
	      domatch_idarray(s, SOLVABLE_RECOMMENDS, md, repo->idarraydata + s->recommends);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_SUPPLEMENTS:
	    if (s->supplements)
	      domatch_idarray(s, SOLVABLE_SUPPLEMENTS, md, repo->idarraydata + s->supplements);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_SUGGESTS:
	    if (s->suggests)
	      domatch_idarray(s, SOLVABLE_SUGGESTS, md, repo->idarraydata + s->suggests);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case SOLVABLE_ENHANCES:
	    if (s->enhances)
	      domatch_idarray(s, SOLVABLE_ENHANCES, md, repo->idarraydata + s->enhances);
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	  case RPM_RPMDBID:
	    if (repo->rpmdbid)
	      {
		kv.num = (unsigned int)repo->rpmdbid[p - repo->start];
		kv.num2 = 0;
		repo_matchvalue(md, s, 0, repo_solvablekeys + (RPM_RPMDBID - SOLVABLE_NAME), &kv);
	      }
	    if (keyname || md->stop > SEARCH_NEXT_KEY)
	      return;
	    break;
	  default:
	    break;
	}
    }

  if (keyname)
    {
      if (keyname == SOLVABLE_FILELIST)
	data = repo_lookup_filelist_repodata(repo, p, &md->matcher);
      else
        data = repo_lookup_repodata_opt(repo, p, keyname);
      if (data)
        repodata_search(data, p, keyname, md->flags, repo_matchvalue, md);
      return;
    }

  keyskip = repo_create_keyskip(repo, p, &md->keyskip);
  FOR_REPODATAS(repo, i, data)
    {
      if (p != SOLVID_META && (p < data->start || p >= data->end))
	continue;
      repodata_search_keyskip(data, p, keyname, md->flags, keyskip, repo_matchvalue, md);
      if (md->stop > SEARCH_NEXT_KEY)
	break;
    }
}

void
repo_search(Repo *repo, Id p, Id keyname, const char *match, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv), void *cbdata)
{
  struct matchdata md;

  if (repo->disabled && !(flags & SEARCH_DISABLED_REPOS))
    return;
  memset(&md, 0, sizeof(md));
  md.pool = repo->pool;
  md.flags = flags;
  md.callback = callback;
  md.callback_data = cbdata;
  if (match)
    datamatcher_init(&md.matcher, match, flags);
  repo_search_md(repo, p, keyname, &md);
  if (match)
    datamatcher_free(&md.matcher);
  solv_free(md.keyskip);
}

Repodata *
repo_lookup_repodata(Repo *repo, Id entry, Id keyname)
{
  Repodata *data;
  int rdid;
  Id type;

  if (entry == SOLVID_POS)
    {
      Pool *pool = repo->pool;
      return pool->pos.repo == repo && pool->pos.repodataid ? pool->pos.repo->repodata + pool->pos.repodataid : 0;
    }
  for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > 0; rdid--, data--)
    {
      if (entry != SOLVID_META && (entry < data->start || entry >= data->end))
	continue;
      if (!repodata_precheck_keyname(data, keyname))
	continue;
      if ((type = repodata_lookup_type(data, entry, keyname)) != 0)
        return type == REPOKEY_TYPE_DELETED ? 0 : data;
    }
  return 0;
}

/* like repo_lookup_repodata, but may return a repodata that contains no match instead of NULL */
Repodata *
repo_lookup_repodata_opt(Repo *repo, Id entry, Id keyname)
{
  Repodata *data, *found = 0;
  int rdid;
  Id type;

  if (entry == SOLVID_POS)
    {
      Pool *pool = repo->pool;
      return pool->pos.repo == repo && pool->pos.repodataid ? pool->pos.repo->repodata + pool->pos.repodataid : 0;
    }
  for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > 0; rdid--, data--)
    {
      if (entry != SOLVID_META && (entry < data->start || entry >= data->end))
	continue;
      if (!repodata_precheck_keyname(data, keyname))
	continue;
      if (found && (type = repodata_lookup_type(found, entry, keyname)) != 0)
	return type == REPOKEY_TYPE_DELETED ? 0 : found;
      found = data;
    }
  return found;
}

Repodata *
repo_lookup_filelist_repodata(Repo *repo, Id entry, Datamatcher *matcher)
{
  Repodata *data;
  int haveextension;
  int rdid;
  Id type;

  if (entry <= 0 || !matcher || !matcher->match || ((matcher->flags & (SEARCH_STRINGMASK|SEARCH_NOCASE)) != SEARCH_STRING
      && (matcher->flags & (SEARCH_STRINGMASK|SEARCH_NOCASE)) != SEARCH_GLOB))
    return repo_lookup_repodata_opt(repo, entry, SOLVABLE_FILELIST);	/* cannot use filtered filelist */

  haveextension = 0;
  for (rdid = repo->nrepodata - 1, data = repo->repodata + rdid; rdid > 0; rdid--, data--)
    {    
      if (entry < data->start || entry >= data->end)
	continue;
      if (data->filelisttype == REPODATA_FILELIST_FILTERED)
	{
	  if (data->state != REPODATA_AVAILABLE)
	    {
	      if (data->state != REPODATA_STUB)
		continue;
	      repodata_load(data);
	      if (data->state != REPODATA_AVAILABLE || entry < data->start || entry >= data->end)
		continue;
	    }
	  /* does this contain any data about the solvable we're looking for? */
	  if (!data->incoreoffset[entry - data->start])
	    continue;	/* no, ignore */
	  if (haveextension && repodata_filelistfilter_matches(data, matcher->match))
	    return data;
	  break;	/* fall back to normal code */
	}
      if (!repodata_has_keyname(data, SOLVABLE_FILELIST))
	continue;
      if (data->filelisttype == REPODATA_FILELIST_EXTENSION)
	{
	  haveextension++;
	  continue;
	}
      if ((type = repodata_lookup_type(data, entry, SOLVABLE_FILELIST)) != 0)
	{
	  if (haveextension)
	    break;		/* need to look in extension */
	  return type == REPOKEY_TYPE_DELETED ? 0 : data;
	}
    }
  /* cannot use filtered filelist */
  return repo_lookup_repodata_opt(repo, entry, SOLVABLE_FILELIST);
}


/* the keyskip array has the following format:
 * 0: keyname area size
 * 1: repoid base
 * 2: repoid end
 * 3: entry for keyname 0
 * 4: entry for keyname 1
 * ...
 */
Id *
repo_create_keyskip(Repo *repo, Id entry, Id **oldkeyskip)
{
  Repodata *data, *last = 0;
  Id *keyskip;
  int rdid, cnt = 0;

  if (repo->nrepodata <= 2)
    return 0;	/* just one repodata, nothing to filter */
  keyskip = oldkeyskip ? *oldkeyskip : 0;
  if (keyskip)
    {
      if (keyskip[1] >= 0x10000000)
	keyskip = solv_free(keyskip);
      else
        keyskip[1] = keyskip[2];
    }
  FOR_REPODATAS(repo, rdid, data)
    {
      if (entry != SOLVID_META)
	{
	  if (data->state != REPODATA_AVAILABLE && data->state != REPODATA_LOADING)
	    {
	      if (data->state != REPODATA_STUB)
		continue;
	      repodata_load(data);
	      if (data->state != REPODATA_AVAILABLE)
		continue;
	    }
	  if ((entry < data->start || entry >= data->end))
	    continue;
	  if (!data->incoreoffset[entry - data->start])
            continue;
	}
      if (last)
        keyskip = repodata_fill_keyskip(last, entry, keyskip);
      last = data;
      cnt++;
    }
  if (cnt <= 1)
    {
      if (oldkeyskip)
	*oldkeyskip = keyskip;
      return 0;
    }
  keyskip = repodata_fill_keyskip(last, entry, keyskip);
  if (keyskip)
    keyskip[2] = keyskip[1] + repo->nrepodata;
  if (oldkeyskip)
    *oldkeyskip = keyskip;
  return keyskip;
}

const char *
repo_lookup_str(Repo *repo, Id entry, Id keyname)
{
  Repodata *data;

  if (entry >= 0)
    {
      Pool *pool = repo->pool;
      switch (keyname)
	{
	case SOLVABLE_NAME:
	  return pool_id2str(pool, pool->solvables[entry].name);
	case SOLVABLE_ARCH:
	  return pool_id2str(pool, pool->solvables[entry].arch);
	case SOLVABLE_EVR:
	  return pool_id2str(pool, pool->solvables[entry].evr);
	case SOLVABLE_VENDOR:
	  return pool_id2str(pool, pool->solvables[entry].vendor);
	}
    }
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  return data ? repodata_lookup_str(data, entry, keyname) : 0;
}


unsigned long long
repo_lookup_num(Repo *repo, Id entry, Id keyname, unsigned long long notfound)
{
  Repodata *data;

  if (entry >= 0)
    {
      if (keyname == RPM_RPMDBID)
	{
	  if (repo->rpmdbid && entry >= repo->start && entry < repo->end)
	    return (unsigned int)repo->rpmdbid[entry - repo->start];
	  return notfound;
	}
    }
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  return data ? repodata_lookup_num(data, entry, keyname, notfound) : notfound;
}

Id
repo_lookup_id(Repo *repo, Id entry, Id keyname)
{
  Repodata *data;
  Id id;

  if (entry >= 0)
    {
      switch (keyname)
	{
	case SOLVABLE_NAME:
	  return repo->pool->solvables[entry].name;
	case SOLVABLE_ARCH:
	  return repo->pool->solvables[entry].arch;
	case SOLVABLE_EVR:
	  return repo->pool->solvables[entry].evr;
	case SOLVABLE_VENDOR:
	  return repo->pool->solvables[entry].vendor;
	}
    }
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data && (id = repodata_lookup_id(data, entry, keyname)) != 0)
    return data->localpool ? repodata_globalize_id(data, id, 1) : id;
  return 0;
}

int
repo_lookup_idarray(Repo *repo, Id entry, Id keyname, Queue *q)
{
  Repodata *data;
  int i;
  if (entry >= 0)
    {
      Offset *offp;
      switch (keyname)
        {
	case SOLVABLE_PROVIDES:
	case SOLVABLE_OBSOLETES:
	case SOLVABLE_CONFLICTS:
	case SOLVABLE_REQUIRES:
	case SOLVABLE_RECOMMENDS:
	case SOLVABLE_SUGGESTS:
	case SOLVABLE_SUPPLEMENTS:
	case SOLVABLE_ENHANCES:
	  offp = solvable_offsetptr(repo->pool->solvables + entry, keyname);
	  if (*offp)
	    {
	      Id *p;
	      for (p = repo->idarraydata + *offp; *p; p++)
	        queue_push(q, *p);
	    }
	  return 1;
        }
    }
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data && repodata_lookup_idarray(data, entry, keyname, q))
    {
      if (data->localpool)
	{
	  for (i = 0; i < q->count; i++)
	    q->elements[i] = repodata_globalize_id(data, q->elements[i], 1);
	}
      return 1;
    }
  queue_empty(q);
  return 0;
}

int
repo_lookup_deparray(Repo *repo, Id entry, Id keyname, Queue *q, Id marker)
{
  int r = repo_lookup_idarray(repo, entry, keyname, q);
  if (!r)
    return 0;
  if (marker == -1 || marker == 1)
    marker = solv_depmarker(keyname, marker);
  if (marker && q->count)
    {
      int i;
      if (marker < 0)
	{
	  marker = -marker;
	  for (i = 0; i < q->count; i++)
	    if (q->elements[i] == marker)
	      {
		queue_truncate(q, i);
		return r;
	      }
	}
      else
	{
	  for (i = 0; i < q->count; i++)
	    if (q->elements[i] == marker)
	      {
		queue_deleten(q, 0, i + 1);
		return r;
	      }
	  queue_empty(q);
	}
    }
  return r;
}

const unsigned char *
repo_lookup_bin_checksum(Repo *repo, Id entry, Id keyname, Id *typep)
{
  const unsigned char *chk;
  Repodata *data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data && (chk = repodata_lookup_bin_checksum(data, entry, keyname, typep)) != 0)
    return chk;
  *typep = 0;
  return 0;
}

const char *
repo_lookup_checksum(Repo *repo, Id entry, Id keyname, Id *typep)
{
  const unsigned char *chk = repo_lookup_bin_checksum(repo, entry, keyname, typep);
  return chk ? pool_bin2hex(repo->pool, chk, solv_chksum_len(*typep)) : 0;
}

int
repo_lookup_void(Repo *repo, Id entry, Id keyname)
{
  Repodata *data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data)
    return repodata_lookup_void(data, entry, keyname);
  return 0;
}

Id
repo_lookup_type(Repo *repo, Id entry, Id keyname)
{
  Id type;
  Repodata *data;
  if (keyname >= SOLVABLE_NAME && keyname <= RPM_RPMDBID)
    return repo_solvablekeys[keyname - SOLVABLE_NAME].type;
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data && (type = repodata_lookup_type(data, entry, keyname)) != 0 && type != REPOKEY_TYPE_DELETED)
    return type;
  return 0;
}

const void *
repo_lookup_binary(Repo *repo, Id entry, Id keyname, int *lenp)
{
  const void *bin;
  Repodata *data = repo_lookup_repodata_opt(repo, entry, keyname);
  if (data && (bin = repodata_lookup_binary(data, entry, keyname, lenp)) != 0)
    return bin;
  *lenp = 0;
  return 0;
}

unsigned int
repo_lookup_count(Repo *repo, Id entry, Id keyname)
{
  Repodata *data;
  if (keyname >= SOLVABLE_NAME && keyname <= RPM_RPMDBID)
  if (entry >= 0 && keyname >= SOLVABLE_NAME && keyname <= RPM_RPMDBID)
    {
      Id *p;
      Offset *offp;
      unsigned int cnt;
      switch (keyname)
        {
	case SOLVABLE_PROVIDES:
	case SOLVABLE_OBSOLETES:
	case SOLVABLE_CONFLICTS:
	case SOLVABLE_REQUIRES:
	case SOLVABLE_RECOMMENDS:
	case SOLVABLE_SUGGESTS:
	case SOLVABLE_SUPPLEMENTS:
	case SOLVABLE_ENHANCES:
	  offp = solvable_offsetptr(repo->pool->solvables + entry, keyname);
	  for (cnt = 0, p = repo->idarraydata + *offp; *p; p++)
	    cnt++;
	  return cnt;
        }
      return 1;
    }
  data = repo_lookup_repodata_opt(repo, entry, keyname);
  return data ? repodata_lookup_count(data, entry, keyname) : 0;
}

/***********************************************************************/

Repodata *
repo_add_repodata(Repo *repo, int flags)
{
  Repodata *data;
  int i;
  if ((flags & REPO_USE_LOADING) != 0)
    {
      for (i = repo->nrepodata - 1; i > 0; i--)
	if (repo->repodata[i].state == REPODATA_LOADING)
	  {
	    Repodata *data = repo->repodata + i;
	    /* re-init */
	    /* hack: we mis-use REPO_REUSE_REPODATA here */
	    if (!(flags & REPO_REUSE_REPODATA))
	      repodata_empty(data, (flags & REPO_LOCALPOOL) ? 1 : 0);
	    return data;
	  }
      return 0;	/* must not create a new repodata! */
    }
  if ((flags & REPO_REUSE_REPODATA) != 0)
    {
      for (i = repo->nrepodata - 1; i > 0; i--)
	if (repo->repodata[i].state != REPODATA_STUB)
	  return repo->repodata + i;
    }
  if (!repo->nrepodata)
    {
      repo->nrepodata = 2;      /* start with id 1 */
      repo->repodata = solv_calloc(repo->nrepodata, sizeof(*data));
    }
  else
    {
      repo->nrepodata++;
      repo->repodata = solv_realloc2(repo->repodata, repo->nrepodata, sizeof(*data));
    }
  data = repo->repodata + repo->nrepodata - 1;
  repodata_initdata(data, repo, (flags & REPO_LOCALPOOL) ? 1 : 0);
  return data;
}

Repodata *
repo_id2repodata(Repo *repo, Id id)
{
  return id ? repo->repodata + id : 0;
}

Repodata *
repo_last_repodata(Repo *repo)
{
  int i;
  for (i = repo->nrepodata - 1; i > 0; i--)
    if (repo->repodata[i].state != REPODATA_STUB)
      return repo->repodata + i;
  return repo_add_repodata(repo, 0);
}

void
repo_set_id(Repo *repo, Id p, Id keyname, Id id)
{
  Repodata *data;
  if (p >= 0)
    {
      switch (keyname)
	{
	case SOLVABLE_NAME:
	  repo->pool->solvables[p].name = id;
	  return;
	case SOLVABLE_ARCH:
	  repo->pool->solvables[p].arch = id;
	  return;
	case SOLVABLE_EVR:
	  repo->pool->solvables[p].evr = id;
	  return;
	case SOLVABLE_VENDOR:
	  repo->pool->solvables[p].vendor = id;
	  return;
	}
    }
  data = repo_last_repodata(repo);
  if (data->localpool)
    id = repodata_localize_id(data, id, 1);
  repodata_set_id(data, p, keyname, id);
}

void
repo_set_num(Repo *repo, Id p, Id keyname, unsigned long long num)
{
  Repodata *data;
  if (p >= 0)
    {
      if (keyname == RPM_RPMDBID)
	{
	  if (!repo->rpmdbid)
	    repo->rpmdbid = repo_sidedata_create(repo, sizeof(Id));
	  repo->rpmdbid[p - repo->start] = (Id)num;
	  return;
	}
    }
  data = repo_last_repodata(repo);
  repodata_set_num(data, p, keyname, num);
}

void
repo_set_str(Repo *repo, Id p, Id keyname, const char *str)
{
  Repodata *data;
  if (p >= 0)
    {
      switch (keyname)
	{
	case SOLVABLE_NAME:
	case SOLVABLE_ARCH:
	case SOLVABLE_EVR:
	case SOLVABLE_VENDOR:
	  repo_set_id(repo, p, keyname, pool_str2id(repo->pool, str, 1));
	  return;
	}
    }
  data = repo_last_repodata(repo);
  repodata_set_str(data, p, keyname, str);
}

void
repo_set_poolstr(Repo *repo, Id p, Id keyname, const char *str)
{
  Repodata *data;
  if (p >= 0)
    {
      switch (keyname)
	{
	case SOLVABLE_NAME:
	case SOLVABLE_ARCH:
	case SOLVABLE_EVR:
	case SOLVABLE_VENDOR:
	  repo_set_id(repo, p, keyname, pool_str2id(repo->pool, str, 1));
	  return;
	}
    }
  data = repo_last_repodata(repo);
  repodata_set_poolstr(data, p, keyname, str);
}

void
repo_add_poolstr_array(Repo *repo, Id p, Id keyname, const char *str)
{
  Repodata *data = repo_last_repodata(repo);
  repodata_add_poolstr_array(data, p, keyname, str);
}

void
repo_add_deparray(Repo *repo, Id p, Id keyname, Id dep, Id marker)
{
  Repodata *data;
  if (marker == -1 || marker == 1)
    marker = solv_depmarker(keyname, marker);
  if (p >= 0)
    {
      Offset *offp;
      switch (keyname)
	{
	case SOLVABLE_PROVIDES:
	case SOLVABLE_OBSOLETES:
	case SOLVABLE_CONFLICTS:
	case SOLVABLE_REQUIRES:
	case SOLVABLE_RECOMMENDS:
	case SOLVABLE_SUGGESTS:
	case SOLVABLE_SUPPLEMENTS:
	case SOLVABLE_ENHANCES:
	  offp = solvable_offsetptr(repo->pool->solvables + p, keyname);
	  *offp = repo_addid_dep(repo, *offp, dep, marker);
	  return;
	}
    }
  data = repo_last_repodata(repo);
  repodata_add_idarray(data, p, keyname, dep);
}

void
repo_add_idarray(Repo *repo, Id p, Id keyname, Id id)
{
  repo_add_deparray(repo, p, keyname, id, 0);
}

void
repo_set_deparray(Repo *repo, Id p, Id keyname, Queue *q, Id marker)
{
  Repodata *data;
  if (marker == -1 || marker == 1)
    marker = solv_depmarker(keyname, marker);
  if (marker)
    {
      /* complex case, splice old and new arrays */
      int i;
      Queue q2;
      queue_init(&q2);
      repo_lookup_deparray(repo, p, keyname, &q2, -marker);
      if (marker > 0)
	{
	  if (q->count)
	    {
	      queue_push(&q2, marker);
	      for (i = 0; i < q->count; i++)
		queue_push(&q2, q->elements[i]);
	    }
	}
      else
	{
	  if (q2.count)
	    queue_insert(&q2, 0, -marker);
	  queue_insertn(&q2, 0, q->count, q->elements);
	}
      repo_set_deparray(repo, p, keyname, &q2, 0);
      queue_free(&q2);
      return;
    }
  if (p >= 0)
    {
      Offset off, *offp;
      int i;
      switch (keyname)
	{
	case SOLVABLE_PROVIDES:
	case SOLVABLE_OBSOLETES:
	case SOLVABLE_CONFLICTS:
	case SOLVABLE_REQUIRES:
	case SOLVABLE_RECOMMENDS:
	case SOLVABLE_SUGGESTS:
	case SOLVABLE_SUPPLEMENTS:
	case SOLVABLE_ENHANCES:
	  off = 0;
	  for (i = 0; i < q->count; i++)
	    off = repo_addid_dep(repo, off, q->elements[i], 0);
	  offp = solvable_offsetptr(repo->pool->solvables + p, keyname);
	  *offp = off;
	  return;
	}
    }
  data = repo_last_repodata(repo);
  repodata_set_idarray(data, p, keyname, q);
}

void
repo_set_idarray(Repo *repo, Id p, Id keyname, Queue *q)
{
  repo_set_deparray(repo, p, keyname, q, 0);
}

void
repo_unset(Repo *repo, Id p, Id keyname)
{
  Repodata *data;
  if (p >= 0)
    {
      Solvable *s = repo->pool->solvables + p;
      switch (keyname)
	{
	case SOLVABLE_NAME:
	  s->name = 0;
	  return;
	case SOLVABLE_ARCH:
	  s->arch = 0;
	  return;
	case SOLVABLE_EVR:
	  s->evr = 0;
	  return;
	case SOLVABLE_VENDOR:
	  s->vendor = 0;
	  return;
        case RPM_RPMDBID:
	  if (repo->rpmdbid)
	    repo->rpmdbid[p - repo->start] = 0;
	  return;
	case SOLVABLE_PROVIDES:
	  s->provides = 0;
	  return;
	case SOLVABLE_OBSOLETES:
	  s->obsoletes = 0;
	  return;
	case SOLVABLE_CONFLICTS:
	  s->conflicts = 0;
	  return;
	case SOLVABLE_REQUIRES:
	  s->requires = 0;
	  return;
	case SOLVABLE_RECOMMENDS:
	  s->recommends = 0;
	  return;
	case SOLVABLE_SUGGESTS:
	  s->suggests = 0;
	  return;
	case SOLVABLE_SUPPLEMENTS:
	  s->supplements = 0;
	case SOLVABLE_ENHANCES:
	  s->enhances = 0;
	  return;
	default:
	  break;
	}
    }
  data = repo_last_repodata(repo);
  repodata_unset(data, p, keyname);
}

void
repo_internalize(Repo *repo)
{
  int i;
  Repodata *data;

  FOR_REPODATAS(repo, i, data)
    if (data->attrs || data->xattrs)
      repodata_internalize(data);
}

void
repo_disable_paging(Repo *repo)
{
  int i;
  Repodata *data;

  FOR_REPODATAS(repo, i, data)
    repodata_disable_paging(data);
}


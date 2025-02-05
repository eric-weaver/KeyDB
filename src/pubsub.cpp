/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

int clientSubscriptionsCount(client *c);

/*-----------------------------------------------------------------------------
 * Pubsub client replies API
 *----------------------------------------------------------------------------*/

/* Send a pubsub message of type "message" to the client. */
void addReplyPubsubMessage(client *c, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReplyAsync(c,shared.mbulkhdr[3]);
    else
        addReplyPushLenAsync(c,3);
    addReplyAsync(c,shared.messagebulk);
    addReplyBulkAsync(c,channel);
    addReplyBulkAsync(c,msg);
}

/* Send a pubsub message of type "pmessage" to the client. The difference
 * with the "message" type delivered by addReplyPubsubMessage() is that
 * this message format also includes the pattern that matched the message. */
void addReplyPubsubPatMessage(client *c, robj *pat, robj *channel, robj *msg) {
    if (c->resp == 2)
        addReplyAsync(c,shared.mbulkhdr[4]);
    else
        addReplyPushLenAsync(c,4);
    addReplyAsync(c,shared.pmessagebulk);
    addReplyBulkAsync(c,pat);
    addReplyBulkAsync(c,channel);
    addReplyBulkAsync(c,msg);
}

/* Send the pubsub subscription notification to the client. */
void addReplyPubsubSubscribed(client *c, robj *channel) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.subscribebulk);
    addReplyBulk(c,channel);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub unsubscription notification to the client.
 * Channel can be NULL: this is useful when the client sends a mass
 * unsubscribe command but there are no channels to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubUnsubscribed(client *c, robj *channel) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.unsubscribebulk);
    if (channel)
        addReplyBulk(c,channel);
    else
        addReplyNull(c, shared.nullbulk);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub pattern subscription notification to the client. */
void addReplyPubsubPatSubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/* Send the pubsub pattern unsubscription notification to the client.
 * Pattern can be NULL: this is useful when the client sends a mass
 * punsubscribe command but there are no pattern to unsubscribe from: we
 * still send a notification. */
void addReplyPubsubPatUnsubscribed(client *c, robj *pattern) {
    if (c->resp == 2)
        addReply(c,shared.mbulkhdr[3]);
    else
        addReplyPushLen(c,3);
    addReply(c,shared.punsubscribebulk);
    if (pattern)
        addReplyBulk(c,pattern);
    else
        addReplyNull(c, shared.nullbulk);
    addReplyLongLong(c,clientSubscriptionsCount(c));
}

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/

void freePubsubPattern(const void *p) {
    pubsubPattern *pat = (pubsubPattern *)p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = (pubsubPattern*)a, *pb = (pubsubPattern*)b;

    return (pa->pclient == pb->pclient) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Return the number of channels + patterns a client is subscribed to. */
int clientSubscriptionsCount(client *c) {
    return dictSize(c->pubsub_channels)+
           listLength(c->pubsub_patterns);
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. */
int pubsubSubscribeChannel(client *c, robj *channel) {
    serverAssert(GlobalLocksAcquired());
    serverAssert(c->lock.fOwnLock());
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    /* Add the channel to the client -> channels hash table */
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);
        /* Add the client to the channel -> list of clients hash table */
        de = dictFind(g_pserver->pubsub_channels,channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(g_pserver->pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = (list*)dictGetVal(de);
        }
        listAddNodeTail(clients,c);
    }
    /* Notify the client */
    addReplyPubsubSubscribed(c,channel);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribeChannel(client *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {
        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        de = dictFind(g_pserver->pubsub_channels,channel);
        serverAssertWithInfo(c,NULL,de != NULL);
        clients = (list*)dictGetVal(de);
        ln = listSearchKey(clients,c);
        serverAssertWithInfo(c,NULL,ln != NULL);
        listDelNode(clients,ln);
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(g_pserver->pubsub_channels,channel);
        }
    }
    /* Notify the client */
    if (notify) addReplyPubsubUnsubscribed(c,channel);
    decrRefCount(channel); /* it is finally safe to release it */
    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. */
int pubsubSubscribePattern(client *c, robj *pattern) {
    serverAssert(GlobalLocksAcquired());
    int retval = 0;

    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        retval = 1;
        pubsubPattern *pat;
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        pat = (pubsubPattern*)zmalloc(sizeof(*pat), MALLOC_LOCAL);
        pat->pattern = getDecodedObject(pattern);
        pat->pclient = c;
        listAddNodeTail(g_pserver->pubsub_patterns,pat);
    }
    /* Notify the client */
    addReplyPubsubPatSubscribed(c,pattern);
    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. */
int pubsubUnsubscribePattern(client *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {
        retval = 1;
        listDelNode(c->pubsub_patterns,ln);
        pat.pclient = c;
        pat.pattern = pattern;
        ln = listSearchKey(g_pserver->pubsub_patterns,&pat);
        listDelNode(g_pserver->pubsub_patterns,ln);
    }
    /* Notify the client */
    if (notify) addReplyPubsubPatUnsubscribed(c,pattern);
    decrRefCount(pattern);
    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed to. */
int pubsubUnsubscribeAllChannels(client *c, int notify) {
    serverAssert(GlobalLocksAcquired());
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;

    while((de = dictNext(di)) != NULL) {
        robj *channel = (robj*)dictGetKey(de);

        count += pubsubUnsubscribeChannel(c,channel,notify);
    }
    /* We were subscribed to nothing? Still reply to the client. */
    if (notify && count == 0) addReplyPubsubUnsubscribed(c,NULL);
    dictReleaseIterator(di);
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. */
int pubsubUnsubscribeAllPatterns(client *c, int notify) {
    serverAssert(GlobalLocksAcquired());
    listNode *ln;
    listIter li;
    int count = 0;

    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = (robj*)ln->value;

        count += pubsubUnsubscribePattern(c,pattern,notify);
    }
    if (notify && count == 0) addReplyPubsubPatUnsubscribed(c,NULL);
    return count;
}

/* Publish a message */
int pubsubPublishMessage(robj *channel, robj *message) {
    serverAssert(GlobalLocksAcquired());
    int receivers = 0;
    dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    de = dictFind(g_pserver->pubsub_channels,channel);
    if (de) {
        list *list = reinterpret_cast<::list*>(dictGetVal(de));
        listNode *ln;
        listIter li;

        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            client *c = reinterpret_cast<client*>(ln->value);
            if (c->flags & CLIENT_CLOSE_ASAP)   // avoid blocking if the write will be ignored
                continue;
            fastlock_lock(&c->lock);
            addReplyPubsubMessage(c,channel,message);
            fastlock_unlock(&c->lock);
            receivers++;
        }
    }
    /* Send to clients listening to matching channels */
    if (listLength(g_pserver->pubsub_patterns)) {
        listRewind(g_pserver->pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {
            pubsubPattern *pat = (pubsubPattern*)ln->value;

            if (stringmatchlen((char*)ptrFromObj(pat->pattern),
                                sdslen(szFromObj(pat->pattern)),
                                (char*)ptrFromObj(channel),
                                sdslen(szFromObj(channel)),0))
            {
                if (pat->pclient->flags & CLIENT_CLOSE_ASAP)
                    continue;
                fastlock_lock(&pat->pclient->lock);
                addReplyPubsubPatMessage(pat->pclient,
                    pat->pattern,channel,message);
                fastlock_unlock(&pat->pclient->lock);
                receivers++;
            }
        }
        decrRefCount(channel);
    }
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

void subscribeCommand(client *c) {
    int j;
    serverAssert(GlobalLocksAcquired());

    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}

void unsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

void psubscribeCommand(client *c) {
    int j;
    serverAssert(GlobalLocksAcquired());

    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= CLIENT_PUBSUB;
}

void punsubscribeCommand(client *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
    if (clientSubscriptionsCount(c) == 0) c->flags &= ~CLIENT_PUBSUB;
}

void publishCommand(client *c) {
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]);
    if (g_pserver->cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
        forceCommandPropagation(c,PROPAGATE_REPL);
    addReplyLongLong(c,receivers);
}

/* PUBSUB command for Pub/Sub introspection. */
void pubsubCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(szFromObj(c->argv[1]),"help")) {
        const char *help[] = {
"CHANNELS [<pattern>] -- Return the currently active channels matching a pattern (default: all).",
"NUMPAT -- Return number of subscriptions to patterns.",
"NUMSUB [channel-1 .. channel-N] -- Returns the number of subscribers for the specified channels (excluding patterns, default: none).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"channels") &&
        (c->argc == 2 || c->argc == 3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        sds pat = (c->argc == 2) ? NULL : szFromObj(c->argv[2]);
        dictIterator *di = dictGetIterator(g_pserver->pubsub_channels);
        dictEntry *de;
        long mblen = 0;
        void *replylen;

        replylen = addReplyDeferredLen(c);
        while((de = dictNext(di)) != NULL) {
            robj *cobj = (robj*)dictGetKey(de);
            sds channel = szFromObj(cobj);

            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        dictReleaseIterator(di);
        setDeferredArrayLen(c,replylen,mblen);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"numsub") && c->argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyArrayLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {
            list *l = (list*)dictFetchValue(g_pserver->pubsub_channels,c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            addReplyLongLong(c,l ? listLength(l) : 0);
        }
    } else if (!strcasecmp(szFromObj(c->argv[1]),"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */
        addReplyLongLong(c,listLength(g_pserver->pubsub_patterns));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

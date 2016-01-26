/**
 * @file http.cpp
 * @brief Generic host HTTP I/O interface
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/http.h"
#include "mega/megaclient.h"
#include "mega/logging.h"

namespace mega {

#ifdef _WIN32
const char* mega_inet_ntop(int af, const void* src, char* dst, int cnt)
{
    wchar_t ip[INET6_ADDRSTRLEN];
    int len = INET6_ADDRSTRLEN;
    int ret = 1;

    if (af == AF_INET)
    {
        struct sockaddr_in in = {};
        in.sin_family = AF_INET;
        memcpy(&in.sin_addr, src, sizeof(struct in_addr));
        ret = WSAAddressToString((struct sockaddr*) &in, sizeof(struct sockaddr_in), 0, ip, (LPDWORD)&len);
    }
    else if (af == AF_INET6)
    {
        struct sockaddr_in6 in = {};
        in.sin6_family = AF_INET6;
        memcpy(&in.sin6_addr, src, sizeof(struct in_addr6));
        ret = WSAAddressToString((struct sockaddr*) &in, sizeof(struct sockaddr_in6), 0, ip, (LPDWORD)&len);
    }

    if (ret != 0)
    {
        return NULL;
    }

    if (!WideCharToMultiByte(CP_UTF8, 0, ip, len, dst, cnt, NULL, NULL))
    {
        return NULL;
    }

    return dst;
}
#endif

HttpIO::HttpIO()
{
    success = false;
    noinetds = 0;
    inetback = false;
    lastdata = NEVER;
    chunkedok = true;
}

// signal Internet status - if the Internet was down for more than one minute,
// set the inetback flag to trigger a reconnect
void HttpIO::inetstatus(bool up)
{
    if (up)
    {
        if (noinetds && Waiter::ds - noinetds > 600)
        {
            inetback = true;
        }

        noinetds = 0;
    }
    else if (!noinetds)
    {
        noinetds = Waiter::ds;
    }
}

// returns true once if an outage just ended
bool HttpIO::inetisback()
{
    if(inetback)
    {
        inetback = false;
        return true;
    }

    return false;
}

void HttpIO::getMEGADNSservers(string *dnsservers, bool getfromnetwork)
{
    if (!dnsservers)
    {
        return;
    }

    dnsservers->clear();
    if (getfromnetwork)
    {
        struct addrinfo *aiList = NULL;
        struct addrinfo *hp;

        struct addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;

#ifndef __MINGW32__
        hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG;
#endif

        if (!getaddrinfo("ns.mega.co.nz", NULL, &hints, &aiList))
        {
            hp = aiList;
            while (hp)
            {
                char straddr[INET6_ADDRSTRLEN];
                straddr[0] = 0;

                if (hp->ai_family == AF_INET)
                {
                    sockaddr_in *addr = (sockaddr_in *)hp->ai_addr;
                    mega_inet_ntop(hp->ai_family, &addr->sin_addr, straddr, sizeof(straddr));
                }
                else if(hp->ai_family == AF_INET6)
                {
                    sockaddr_in6 *addr = (sockaddr_in6 *)hp->ai_addr;
                    mega_inet_ntop(hp->ai_family, &addr->sin6_addr, straddr, sizeof(straddr));
                }

                if (straddr[0])
                {
                    if(dnsservers->size())
                    {
                        dnsservers->append(",");
                    }
                    dnsservers->append(straddr);
                }

                hp = hp->ai_next;
            }
            freeaddrinfo(aiList);
        }
    }

    if (!getfromnetwork || !dnsservers->size())
    {
        LOG_info << "Using hardcoded MEGA DNS servers: " << *dnsservers;
        *dnsservers = MEGA_DNS_SERVERS;
    }
    else
    {
        LOG_info << "Using current MEGA DNS servers: " << *dnsservers;
    }
}

void HttpReq::post(MegaClient* client, const char* data, unsigned len)
{
    if (httpio)
    {
        LOG_warn << "Ensuring that the request is finished before sending it again";
        httpio->cancel(this);
    }

    httpio = client->httpio;
    bufpos = 0;
    inpurge = 0;
    contentlength = -1;

    httpio->post(this, data, len);
}

// attempt to send chunked data, remove from out
void HttpReq::postchunked(MegaClient* client)
{
    if (!chunked)
    {
        chunked = true;
        post(client);
    }
    else
    {
        if (httpio)
        {
            httpio->sendchunked(this);
        }
    }
}

void HttpReq::disconnect()
{
    if (httpio)
    {
        httpio->cancel(this);
        httpio = NULL;
    }

    chunked = false;
}

HttpReq::HttpReq(bool b)
{
    binary = b;

    status = REQ_READY;
    httpstatus = 0;
    buf = NULL;

    httpio = NULL;
    httpiohandle = NULL;
    out = &outbuf;

    inpurge = 0;
    
    chunked = false;
    sslcheckfailed = false;

    type = REQ_JSON;
    buflen = 0;
    bufpos = 0;
    contentlength = 0;
    lastdata = 0;
}

HttpReq::~HttpReq()
{
    if (httpio)
    {
        httpio->cancel(this);
    }

    delete[] buf;
}

void HttpReq::setreq(const char* u, contenttype_t t)
{
    if (u)
    {
        posturl = u;
    }

    type = t;
}

// add data to fixed or variable buffer
void HttpReq::put(void* data, unsigned len, bool purge)
{
    if (buf)
    {
        if (bufpos + len > buflen)
        {
            len = buflen - bufpos;
        }

        memcpy(buf + bufpos, data, len);
    }
    else
    {
        if (inpurge && purge)
        {
            in.erase(0, inpurge);
            inpurge = 0;
        }

        in.append((char*)data, len);
    }
    
    bufpos += len;
}

char* HttpReq::data()
{
    return (char*)in.data() + inpurge;
}

size_t HttpReq::size()
{
    return in.size() - inpurge;
}

// set amount of purgeable in data at 0
void HttpReq::purge(size_t numbytes)
{
    inpurge += numbytes;
}

// set total response size
void HttpReq::setcontentlength(m_off_t len)
{
    if (!buf && type != REQ_BINARY)
    {
        in.reserve(len);
    }

    contentlength = len;
}

// make space for receiving data; adjust len if out of space
byte* HttpReq::reserveput(unsigned* len)
{
    if (buf)
    {
        if (bufpos + *len > buflen)
        {
            *len = buflen - bufpos;
        }

        return buf + bufpos;
    }
    else
    {
        if (inpurge)
        {
            // FIXME: optimize erase()/resize() -> single copy/resize()
            in.erase(0, inpurge);
            bufpos -= inpurge;
            inpurge = 0;
        }

        if (bufpos + *len > in.size())
        {
            in.resize(bufpos + *len);
        }

        *len = in.size() - bufpos;

        return (byte*)in.data() + bufpos;
    }
}

// number of bytes transferred in this request
m_off_t HttpReq::transferred(MegaClient*)
{
    if (buf)
    {
        return bufpos;
    }
    else
    {
        return in.size();
    }
}

// prepare file chunk download
void HttpReqDL::prepare(const char* tempurl, SymmCipher* key,
                        chunkmac_map* macs, uint64_t ctriv, m_off_t pos,
                        m_off_t npos)
{
    char urlbuf[256];

    snprintf(urlbuf, sizeof urlbuf, "%s/%" PRIu64 "-%" PRIu64, tempurl, pos, npos - 1);
    setreq(urlbuf, REQ_BINARY);

    dlpos = pos;
    size = (unsigned)(npos - pos);

    if (!buf || buflen != size)
    {
        // (re)allocate buffer
        if (buf)
        {
            delete[] buf;
        }

        buf = new byte[(size + SymmCipher::BLOCKSIZE - 1) & - SymmCipher::BLOCKSIZE];
        buflen = size;
    }
}

// decrypt, mac and write downloaded chunk
void HttpReqDL::finalize(SymmCipher* key, chunkmac_map* macs, uint64_t ctriv)
{
    byte mac[SymmCipher::BLOCKSIZE] = { 0 };

    key->ctr_crypt(buf, bufpos, dlpos, ctriv, mac, 0);

    memcpy((*macs)[dlpos].mac, mac, sizeof mac);
}

// prepare chunk for uploading: mac and encrypt
void HttpReqUL::prepare(const char* tempurl, SymmCipher* key,
                        chunkmac_map* macs, uint64_t ctriv, m_off_t pos,
                        m_off_t npos)
{
    size = (unsigned)(npos - pos);

    byte mac[SymmCipher::BLOCKSIZE] = { 0 };
    char buf[256];

    snprintf(buf, sizeof buf, "%s/%" PRIu64, tempurl, pos);
    setreq(buf, REQ_BINARY);

    key->ctr_crypt((byte*)out->data(), size, pos, ctriv, mac, 1);

    memcpy((*macs)[pos].mac, mac, sizeof mac);

    // unpad for POSTing
    out->resize(size);
}

// number of bytes sent in this request
m_off_t HttpReqUL::transferred(MegaClient* client)
{
    if (httpiohandle)
    {
        return client->httpio->postpos(httpiohandle);
    }

    return 0;
}
} // namespace

#include "mega/osx/osxutils.h"
#include <Cocoa/Cocoa.h>
#include <SystemConfiguration/SystemConfiguration.h>

using namespace mega;

enum { HTTP_PROXY = 0, HTTPS_PROXY };

CFTypeRef getValueFromKey(CFDictionaryRef dict, const void *key, CFTypeID type)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value)
    {
        return NULL;
    }

    if ((CFGetTypeID(value) == type))
    {
        return value;
    }

    return NULL;
}

bool getProxyConfiguration(CFDictionaryRef dict, int proxyType, Proxy* proxy)
{
    int isEnabled = 0;
    int port;

    if (proxyType != HTTPS_PROXY && proxyType != HTTP_PROXY)
    {
        return false;
    }

    CFStringRef proxyEnableKey = proxyType == HTTP_PROXY ? kSCPropNetProxiesHTTPEnable : kSCPropNetProxiesHTTPSEnable;
    CFStringRef proxyHostKey   = proxyType == HTTP_PROXY ? kSCPropNetProxiesHTTPProxy : kSCPropNetProxiesHTTPSProxy;
    CFStringRef portKey        = proxyType == HTTP_PROXY ? kSCPropNetProxiesHTTPPort : kSCPropNetProxiesHTTPSPort;

    CFNumberRef proxyEnabledRef = (CFNumberRef)getValueFromKey(dict, proxyEnableKey, CFNumberGetTypeID());
    if (proxyEnabledRef && CFNumberGetValue(proxyEnabledRef, kCFNumberIntType, &isEnabled) && (isEnabled != 0))
    {
        if ([(NSDictionary*)dict valueForKey: proxyType == HTTP_PROXY ? @"HTTPUser" : @"HTTPSUser"] != nil)
        {
            // Username set, skip proxy configuration. We only allow proxies withouth user/pw credentials
            // to not have to request the password to the user to read the keychain
            return false;
        }

        CFStringRef hostRef = (CFStringRef)getValueFromKey(dict, proxyHostKey, CFStringGetTypeID());
        if (hostRef)
        {
            CFIndex length = CFStringGetLength(hostRef);
            CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
            char *buffer = new char[maxSize];
            if (CFStringGetCString(hostRef, buffer, maxSize, kCFStringEncodingUTF8))
            {
                CFNumberRef portRef = (CFNumberRef)getValueFromKey(dict, portKey, CFNumberGetTypeID());
                if (portRef && CFNumberGetValue(portRef, kCFNumberIntType, &port))
                {
                    ostringstream oss;
                    oss << (proxyType == HTTP_PROXY ? "http://" : "https://") << buffer << ":" << port;
                    string link = oss.str();
                    proxy->setProxyType(Proxy::CUSTOM);
                    proxy->setProxyURL(&link);
                    delete [] buffer;
                    return true;
                }
            }
            delete [] buffer;
        }
    }
    return false;
}

void getOSXproxy(Proxy* proxy)
{
    CFDictionaryRef proxySettings = NULL;
    proxySettings = SCDynamicStoreCopyProxies(NULL);
    if (!proxySettings)
    {
        return;
    }

    if (!getProxyConfiguration(proxySettings, HTTPS_PROXY, proxy))
    {
        getProxyConfiguration(proxySettings, HTTP_PROXY, proxy);
    }
    CFRelease(proxySettings);
}

// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3XML_H_
#define S3XML_H_

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "ObjectStore.h"

/**
 * The few S3 XML documents this server produces, and the one it reads.
 */
class S3Xml
{
    public:
        static std::string escape(const std::string& str)
        {
            std::string escaped;

            for(char c : str)
            {
                switch(c)
                {
                    case '&': escaped += "&amp;"; break;
                    case '<': escaped += "&lt;"; break;
                    case '>': escaped += "&gt;"; break;
                    case '"': escaped += "&quot;"; break;
                    default: escaped += c;
                }
            }

            return escaped;
        }

        static std::string unescape(std::string str)
        {
            for(const auto& [entity, plain] : {std::pair<const char*, const char*>{"&lt;", "<"},
                {"&gt;", ">"}, {"&quot;", "\""}, {"&amp;", "&"} } )
                for(size_t pos; (pos = str.find(entity) ) != std::string::npos; )
                    str.replace(pos, strlen(entity), plain);

            return str;
        }

        // "2026-09-30T12:00:00.000Z"
        static std::string iso8601(time_t time)
        {
            char buf[32];
            struct tm tmBuf;
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S.000Z", gmtime_r(&time, &tmBuf) );
            return buf;
        }

        // "Wed, 30 Sep 2026 12:00:00 GMT"
        static std::string httpDate(time_t time)
        {
            char buf[48];
            struct tm tmBuf;
            strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", gmtime_r(&time, &tmBuf) );
            return buf;
        }

        static std::string error(const std::string& code, const std::string& message,
            const std::string& resource)
        {
            return header() + "<Error><Code>" + code + "</Code><Message>" + escape(message) +
                "</Message><Resource>" + escape(resource) + "</Resource></Error>";
        }

        static std::string listBuckets(const std::vector<std::string>& buckets)
        {
            std::string xml = header() + "<ListAllMyBucketsResult xmlns=\"" + ns() + "\">"
                "<Owner><ID>s3rdma-server</ID><DisplayName>s3rdma-server</DisplayName></Owner><Buckets>";

            for(const std::string& bucket : buckets)
                xml += "<Bucket><Name>" + escape(bucket) + "</Name></Bucket>";

            return xml + "</Buckets></ListAllMyBucketsResult>";
        }

        // ListObjectsV2 result. Everything is returned in one page.
        static std::string listObjects(const std::string& bucket, const std::string& prefix,
            const std::vector<ObjectInfo>& objects)
        {
            std::string xml = header() + "<ListBucketResult xmlns=\"" + ns() + "\">"
                "<Name>" + escape(bucket) + "</Name><Prefix>" + escape(prefix) + "</Prefix>"
                "<KeyCount>" + std::to_string(objects.size() ) + "</KeyCount>"
                "<MaxKeys>" + std::to_string(objects.size() ) + "</MaxKeys>"
                "<IsTruncated>false</IsTruncated>";

            for(const ObjectInfo& object : objects)
                xml += "<Contents><Key>" + escape(object.key) + "</Key>"
                    "<LastModified>" + iso8601(object.mtime) + "</LastModified>"
                    "<ETag>" + escape(object.etag) + "</ETag>"
                    "<Size>" + std::to_string(object.size) + "</Size>"
                    "<StorageClass>STANDARD</StorageClass></Contents>";

            return xml + "</ListBucketResult>";
        }

        static std::string initiateMultipartUpload(const std::string& bucket,
            const std::string& key, const std::string& uploadID)
        {
            return header() + "<InitiateMultipartUploadResult xmlns=\"" + ns() + "\">"
                "<Bucket>" + escape(bucket) + "</Bucket><Key>" + escape(key) + "</Key>"
                "<UploadId>" + uploadID + "</UploadId></InitiateMultipartUploadResult>";
        }

        static std::string completeMultipartUpload(const std::string& bucket,
            const std::string& key, const std::string& etag)
        {
            return header() + "<CompleteMultipartUploadResult xmlns=\"" + ns() + "\">"
                "<Location>/" + escape(bucket) + "/" + escape(key) + "</Location>"
                "<Bucket>" + escape(bucket) + "</Bucket><Key>" + escape(key) + "</Key>"
                "<ETag>" + escape(etag) + "</ETag></CompleteMultipartUploadResult>";
        }

        static std::string deleteObjects(const std::vector<std::string>& keys)
        {
            std::string xml = header() + "<DeleteResult xmlns=\"" + ns() + "\">";

            for(const std::string& key : keys)
                xml += "<Deleted><Key>" + escape(key) + "</Key></Deleted>";

            return xml + "</DeleteResult>";
        }

        // The keys of a DeleteObjects request body ("<Delete><Object><Key>...</Key>...").
        static std::vector<std::string> parseDeleteKeys(const std::string& xml)
        {
            std::vector<std::string> keys;

            for(size_t pos = 0; (pos = xml.find("<Key>", pos) ) != std::string::npos; )
            {
                pos += strlen("<Key>");
                const size_t end = xml.find("</Key>", pos);
                if(end == std::string::npos)
                    break;

                keys.push_back(unescape(xml.substr(pos, end - pos) ) );
                pos = end;
            }

            return keys;
        }

    private:
        S3Xml() {}

        static std::string header() { return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"; }
        static std::string ns() { return "http://s3.amazonaws.com/doc/2006-03-01/"; }
};

#endif // S3XML_H_

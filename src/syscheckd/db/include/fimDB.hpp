/*
 * Wazuh Syscheckd
 * Copyright (C) 2015-2021, Wazuh Inc.
 * September 23, 2021.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef _FIMDB_HPP
#define _FIMDB_HPP
#include "dbsync.hpp"
#include "fimDB.hpp"
#include "dbItem.hpp"
#include "rsync.hpp"
#include "shared.h"

// Define EXPORTED for any platform
#ifdef _WIN32
#ifdef WIN_EXPORT
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __declspec(dllimport)
#endif
#elif __GNUC__ >= 4
#define EXPORTED __attribute__((visibility("default")))
#else
#define EXPORTED
#endif

enum class dbResult {
    DB_SUCCESS,
    DB_ERROR
};

class EXPORTED FIMDB final
{
public:
    static FIMDB& getInstance()
    {
        static FIMDB s_instance;
        return s_instance;
    }

    void init();
    void syncDB();
    int insertItem(DBItem*);
    int removeItem(DBItem*);
    int updateItem(DBItem*);
    int setAllUnscanned();
    int executeQuery();

private:
    FIMDB();
    ~FIMDB() = default;
    FIMDB(const FIMDB&) = delete;
    std::unique_ptr<DBSync>       m_dbsyncHandler;
    std::unique_ptr<RemoteSync>   m_rsyncHandler;
    std::string createStatement();
    void setFileLimit();
    void setRegistryLimit();
    void setValueLimit();

};
#endif //_FIMDB_HPP

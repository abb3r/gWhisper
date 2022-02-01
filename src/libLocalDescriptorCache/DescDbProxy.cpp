// Copyright 2021 IBM Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


/// Check wether get DescDB from local location or via server reflection
/// Input: server Adress, connection List (or 1 Param if possible with connections.[server_adress])
/// Output: Db either of type local or type refectoin. Local should have same interface as reflection (take reflection as reference)
#include "DescDbProxy.hpp"
#include "../utils/gwhisperUtils.hpp"
#include "LocalDescDb.pb.h"

#include<string>
#include <deque>
#include <iostream>
#include <fstream>

#include <versionDefine.h> // generated during build

#include <google/protobuf/descriptor.h>
#include <grpcpp/grpcpp.h>
#include <gRPC_utils/proto_reflection_descriptor_database.h>
#include <google/protobuf/util/time_util.h>
#include <grpcpp/impl/codegen/config_protobuf.h>



bool DescDbProxy::isValidHostEntry(const localDescDb::DescriptorDb& descDb, const std::string hostAddress){
    
    bool validHostEntry=false; //TODO: Maybe rename in something with Reflection
    static google::protobuf::util::TimeUtil timestamp;
    //static google::protobuf::util::TimeUtil lastUpdateTime;
    google::protobuf::Timestamp currentTime;
    google::protobuf::Timestamp lastUpdate;
    int secCurrentTime;
    int secLastUpdate;
    int timeDiff;

    for (int i=0; i < descDb.hosts_size(); i++){
        const localDescDb::Host host = descDb.hosts(i);
        if (host.hostaddress() == hostAddress)
        {
           // Get current Time and Time of last Upate in Seconds:
            currentTime = timestamp.GetCurrentTime(); // time in UTC
            secCurrentTime = timestamp.TimestampToSeconds(currentTime);
            
            lastUpdate = host.lastupdate();
            secLastUpdate = timestamp.TimestampToSeconds(lastUpdate);

            // Calculate time in seconds btw. lastUpdate and currentTime
            timeDiff = secCurrentTime - secLastUpdate;

            if (timeDiff <= 120){
                validHostEntry = true;
            }      
        }

    }
    return validHostEntry;
}

void DescDbProxy::editLocalDb(localDescDb::Host* host, std::string hostAddress,std::shared_ptr<grpc::Channel> channel){

    static google::protobuf::util::TimeUtil timestamp;
    
    host->clear_hostaddress();
    host->clear_lastupdate();
    host->clear_servicelist();
    host->clear_file_descriptor_proto();

    //TODO: Duplicates during fetching or as post-processing 
    //(when write into DB variable, before writing to disk)
    fetchDbFromReflection(host, hostAddress, channel);

    //TODO Neue Funktion: AddEntry():
    host->set_hostaddress(hostAddress);
    //lastUpdate->set_seconds(5); 
    (*(host->mutable_lastupdate())) = timestamp.GetCurrentTime();
    // Add service Naems to DB entry
    for(const auto& serviceName: (serviceList)){
        // Add service to loacal DB
        host->add_servicelist(serviceName); 
    }

    // Add all descriptors to DB entry
    // m_ vor membern
    for (const auto& fileName: (descNames)){
        grpc::protobuf::FileDescriptorProto  output;
        reflectionDescDb.FindFileByName(fileName, &output);
        std::string dbDescEntry = output.SerializeAsString();
        host->add_file_descriptor_proto(dbDescEntry);
    }
}

void fetchDbFromReflection(localDescDb::Host* host, std::string hostAddress,std::shared_ptr<grpc::Channel> channel){

    reflectionDescDb.GetServices(&serviceList);
    google::protobuf::DescriptorPool descPool(&reflectionDescDb);
     
    for(const auto& serviceName: (serviceList)){
        const grpc::protobuf::FileDescriptor * serviceFileDesc;
        const grpc::protobuf::FileDescriptor * dependencyDesc;
        const grpc::protobuf::ServiceDescriptor * serviceDesc;

        // Get service file names through services in descPool
        // Use filenames to retrieve service descriptors from ReflectionDB
        serviceDesc = descPool.FindServiceByName(serviceName);
        if (! serviceDesc){
            // Catching Nullpointer wenn service keinen desc hat
            continue;
        }

        serviceFileDesc = serviceDesc->file();

        // Retrieve all proto files used by the service
        int dependencyCounter = serviceFileDesc->dependency_count();

        descNames.push_back(serviceFileDesc->name());

        //TODO: Loop only, if dependencies weren't already fetched (If parent not yet in descNames)?
        for (int i=0; i<dependencyCounter; i++){
            dependencyDesc = serviceFileDesc->dependency(i);
            // Get file of imported files used in this service and search for more files
            getDependencies(dependencyDesc);
        }
    }
}

void DescDbProxy::getDependencies(const grpc::protobuf::FileDescriptor * f_parentDesc){
        
        std::deque<const grpc::protobuf::FileDescriptor*> todoList;
        std::vector<const grpc::protobuf::FileDescriptor*> doneList;
        int amountChildren;

        todoList.push_front(parentDesc);

        while (not todoList.empty()){
            if(std::find(doneList.begin(), doneList.end(),todoList.front()) != doneList.end()){
                // Discard already processed descriptors
                todoList.pop_front();
            }else{
                amountChildren = todoList.front()->dependency_count();
                for (int c=0; c < amountChildren; c++)
                {    
                    todoList.push_back(todoList.front()->dependency(c));
                }
        
                //const grpc::protobuf::FileDescriptor* test = todoList.front();
                std::string currentFileName = todoList.front()->name();  
                descNames.push_back(currentFileName);
                doneList.push_back(todoList.front());
                todoList.pop_front();    
            }          
        }       
}


void DescDbProxy::getDescriptors(std::string dbFileName, std::string hostAddress, std::shared_ptr<grpc::Channel> channel){
    
    //Use dbFile from .prot file
    localDescDb::DescriptorDb dbFile;
    
    // Read the existing db (should be serialized)
    std::fstream input;
    input.open(dbFileName);
    dbFile.ParseFromIstream(&input); 

    //Add/Update DB entry for new/outdated host (via Reflection)
    //std::string gwhisperBuildVersion = GWHISPER_BUILD_VERSION;
    bool servicesRetrieved = false;
    if(!isValidHostEntry(dbFile, hostAddress)){ // || (dbFile.gwhisper_version() != gwhisperBuildVersion)){
        //TODO: Delete host Entry before writing new
        //dbFile.set_gwhisper_version(gwhisperBuildVersion); //Correct Place to set version?
        editLocalDb(dbFile.add_hosts(), hostAddress, channel);
        servicesRetrieved = true;
        //std::cout << dbFile.DebugString();

    }

    // Add services to localDB variable if not happened in editLocalDB
    // Warum mache ich das?
    std::string serviceName;
    if (!servicesRetrieved){
        for (int i = 0; i < dbFile.hosts_size(); i++){
            const localDescDb::Host host = dbFile.hosts(i); 
            if (host.hostaddress() == hostAddress){
                for(int j=0; j < host.servicelist_size(); j++){
                    serviceName = host.servicelist(j); 
                    (serviceList).push_back(serviceName);
                }
            }     
        }
    }

    // Get Desc for Host and add them to temporary localDB
    // convertHostEntryToSimpleSDescDb
    for (int i=0; i < dbFile.hosts_size(); i++){
        const localDescDb::Host host = dbFile.hosts(i);
        if (host.hostaddress() == hostAddress)
        {
            for (int i=0; i < host.file_descriptor_proto_size(); i++){
                google::protobuf::FileDescriptorProto descriptor;
                google::protobuf::FileDescriptorProto output;
                //std::string descriptor =  host.file_descriptor_proto(i);
                //Here: Error "File already exists in DB", google/protobuf/descriptor_database.cc:120
                //TODO: prevent Duplicates here! (File-already exists-Error) --> better prevent Duplicates in reflection
                descriptor.ParseFromString(host.file_descriptor_proto(i));
                if (!m_localDB.FindFileByName(descriptor.name(), &output)){
                    m_localDB.Add(descriptor);
                }
            }                       
        }
        break;
    }

   // writeCacheToFile(dbFileName, dbFile, localDescDb);
    //Write updated Db to disc --> new function: write Cahce to File
    std::fstream output(dbFileName, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!dbFile.SerializeToOstream(&output)){
        std::cerr << "Failed to write DB." << std::endl;
    }
}


DescDbProxy::DescDbProxy(std::string dbFileName, std::string hostAddress, std::shared_ptr<grpc::Channel> channel) :
    reflectionDescDb(channel)
{
    getDescriptors(dbFileName, hostAddress, channel);   //sollte localDB returnen?
}
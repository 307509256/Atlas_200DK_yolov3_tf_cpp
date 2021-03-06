/**
* Copyright 2020 Huawei Technologies Co., Ltd
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at

* http://www.apache.org/licenses/LICENSE-2.0

* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.

* File sample_process.cpp
* Description: handle acl resource
*/
#include "object_detect.h"
#include <bits/stdint-uintn.h>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <sys/types.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <time.h>
#include "opencv2/imgcodecs/legacy/constants_c.h"
#include "opencv2/opencv.hpp"
#include "acl/acl.h"
#include "model_process.h"
#include "utils.h"

using namespace std;

namespace {

    const static std::vector<std::string> yolov3Label = {"airplane", "ship", "oiltank", "playground", "port", "bridge", "car"};
    /*
    const static std::vector<std::string> yolov3Label = { "person", "bicycle", "car", "motorbike",
    "aeroplane","bus", "train", "truck", "boat",
    "traffic light", "fire hydrant", "stop sign", "parking meter",
    "bench", "bird", "cat", "dog", "horse",
    "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag","tie",
    "suitcase", "frisbee", "skis", "snowboard", "sports ball",
    "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana",
    "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair",
    "sofa", "potted plant", "bed", "dining table", "toilet",
    "TV monitor", "laptop", "mouse", "remote", "keyboard",
    "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase","scissors",
    "teddy bear", "hair drier", "toothbrush" };
    */
    const uint numClasses = 7;
    const uint BoxTensorLabel  = 12;

    const uint numBBoxes = 3;
    const uint  BoxTensorLength = (BoxTensorLabel * numBBoxes);
    const float nmsThresh = 0.45;
    const float MaxBoxClassThresh = 0.3;
    const static vector<uint32_t>  kGridSize = {52,26,13};


    enum BBoxIndex { TOPLEFTX = 0, TOPLEFTY, BOTTOMRIGHTX, BOTTOMRIGHTY, SCORE, LABEL }; //枚举 从0开始依次加1
    // output image prefix
    const string kOutputFilePrefix = "out_";
    // bounding box line solid
    const uint32_t kLineSolid = 2;
    // opencv draw label params.
    const double kFountScale = 0.5;
    const cv::Scalar kFontColor(0, 0, 255);
    const uint32_t kLabelOffset = 11;
    const string kFileSperator = "/";
    // opencv color list for boundingbox
    const vector<cv::Scalar> kColors{
        cv::Scalar(237, 149, 100), cv::Scalar(0, 215, 255), cv::Scalar(50, 205, 50),
        cv::Scalar(139, 85, 26) };

}

ObjectDetect::ObjectDetect(const char* modelPath, uint32_t modelWidth,
                            uint32_t modelHeight)
:deviceId_(0), imageDataBuf_(nullptr), modelWidth_(modelWidth), modelHeight_(modelHeight), isInited_(false){
    modelPath_ = modelPath;
    imageDataSize_ = RGBU8_IMAGE_SIZE(modelWidth, modelHeight);
}

ObjectDetect::~ObjectDetect() {
    DestroyResource();
}

Result ObjectDetect::InitResource() { //相当于acl_resource.py
    // ACL init
    const char *aclConfigPath = "../src/acl.json";
    aclError ret = aclInit(aclConfigPath);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl init failed");
        return FAILED;
    }
    INFO_LOG("acl init success");

    // open device
    ret = aclrtSetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl open device %d failed", deviceId_);
        return FAILED;
    }
    INFO_LOG("open device %d success", deviceId_);

    ret = aclrtGetRunMode(&runMode_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("acl get run mode failed");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::InitModel(const char* omModelPath) {
    Result ret = model_.LoadModelFromFileWithMem(omModelPath);
    if (ret != SUCCESS) {
        ERROR_LOG("execute LoadModelFromFileWithMem failed");
        return FAILED;
    }

    ret = model_.CreateDesc();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateDesc failed");
        return FAILED;
    }

    ret = model_.CreateOutput();
    if (ret != SUCCESS) {
        ERROR_LOG("execute CreateOutput failed");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::CreateModelInputdDataset()
{
    //Request image data memory for input model
    aclError aclRet = aclrtMalloc(&imageDataBuf_, imageDataSize_, ACL_MEM_MALLOC_HUGE_FIRST);
    if (aclRet != ACL_ERROR_NONE) {
        ERROR_LOG("malloc device data buffer failed, aclRet is %d", aclRet);
        return FAILED;
    }
    Result ret = model_.CreateInput(imageDataBuf_, imageDataSize_);
    if (ret != SUCCESS) {
        ERROR_LOG("Create mode input dataset failed");
        return FAILED;
    }
    return SUCCESS;
}

Result ObjectDetect::Init() {
    if (isInited_) {
        INFO_LOG("Classify instance is initied already!");
        return SUCCESS;
    }

    Result ret = InitResource();
    if (ret != SUCCESS) {
        ERROR_LOG("Init acl resource failed");
        return FAILED;
    }

    ret = InitModel(modelPath_);
    if (ret != SUCCESS) {
        ERROR_LOG("Init model failed");
        return FAILED;
    }

    ret = CreateModelInputdDataset();
    if (ret != SUCCESS) {
        ERROR_LOG("Create image info buf failed");
        return FAILED;
    }
    isInited_ = true;
    return SUCCESS;
}

Result ObjectDetect::Preprocess(cv::Mat& frame, uint32_t& W, uint32_t& H){
    float widthScale = float(modelWidth_) / float(W);
    float heightScale = float(modelHeight_) / float(H);
    float Scale = min(widthScale, heightScale);
    uint32_t new_W = Scale * W;
    uint32_t new_H = Scale * H;
    //resize
    cv::Mat reiszeMat;
    cv::resize(frame, reiszeMat, cv::Size(new_W, new_H));
    if (reiszeMat.empty()) {
        ERROR_LOG("Resize image failed");
        return FAILED;
    }
    //padding
    float dw = (float)(modelWidth_ - new_W) / 2.0;
    float dh = (float)(modelHeight_ - new_H) / 2.0;

    int top = (int)Utils::round(dh - 0.1);
    int bottom = (int)Utils::round(dh + 0.1);
    int left = (int)Utils::round(dw - 0.1);
    int right = (int)Utils::round(dw + 0.1);
    //cout << top <<" "<<bottom<<" "<<left<<" "<<right<<endl;
    cv::Mat PaddingMat;
    cv::copyMakeBorder(reiszeMat, PaddingMat, top, bottom, left, right, cv::BORDER_CONSTANT, 0);
    if (PaddingMat.cols != modelWidth_ or PaddingMat.rows != modelHeight_) {
        ERROR_LOG("Padding image failed");
        return FAILED;
    }
    //Copy the data into the cache of the input dataset
    aclrtMemcpyKind policy = (runMode_ == ACL_HOST)? ACL_MEMCPY_HOST_TO_DEVICE:ACL_MEMCPY_DEVICE_TO_DEVICE;
    //实现host内 host与device之间 devices内的同步内存复制
    //目的内存地址指针、目的内存地址的最大内存长度、源内存地址指针、内存复制的长度
    aclError ret = aclrtMemcpy(imageDataBuf_, imageDataSize_, PaddingMat.ptr<uint8_t>(), imageDataSize_, policy);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("Copy padding image data to device failed.");
        return FAILED;
    }

    return SUCCESS;
}

Result ObjectDetect::Inference(aclmdlDataset*& inferenceOutput) {
    Result ret = model_.Execute();
    if (ret != SUCCESS) {
        ERROR_LOG("Execute model inference failed");
        return FAILED;
    }
    inferenceOutput = model_.GetModelOutputData();

    return SUCCESS;
}

vector<BBox> ObjectDetect::Postprocess(aclmdlDataset* modelOutput, uint32_t& W, uint32_t& H) {
    vector<BBox> binfo, bboxesNew;
    for(uint ImgIndex =0; ImgIndex < 3 ;ImgIndex ++)
    {
        uint32_t dataSize = 0;
        uint gridSize = kGridSize[ImgIndex];
        float* detectData = (float *)GetInferenceOutputItem(dataSize, modelOutput, ImgIndex);
        int32_t  size = dataSize / sizeof(double); //eg:52*52*3*85 h,w
        /*
        //show some results
        for(int i = 0; i < gridSize; i++){
            for(int j = 0; j < gridSize; j++){
                for(int k = 0; k < numBBoxes; k++){
                    for(int l = 0; l < BoxTensorLabel; l++){
                        int ind = ((i*gridSize+j)*numBBoxes+k)*BoxTensorLabel+l;
                        cout<<detectData[ind]<<" ";
                    }
                    cout<<endl;
                    break;
                }
                break;
            }
            break;
        }
        */
        //scale transform
        float widthScale = float(modelWidth_) / float(W);
        float heightScale = float(modelHeight_) / float(H);
        float Scale = min(widthScale, heightScale);
        uint32_t new_W = Scale * W;
        uint32_t new_H = Scale * H;
        float dw = (float)(modelWidth_ - new_W) / 2.0;
        float dh = (float)(modelHeight_ - new_H) / 2.0;
        for(uint cx = 0; cx < gridSize; cx++)
        {
            for(uint cy = 0; cy < gridSize; cy++)
            {
                float x;
                float y;
                float w;
                float h;
                float cf;

                for (uint i = 0; i  < numBBoxes; ++i)
                {
                    const int bbindex = ((cx * gridSize + cy) * numBBoxes) + i;
                    x  = detectData[bbindex * BoxTensorLabel + 0];
                    y  = detectData[bbindex * BoxTensorLabel + 1];
                    w  = detectData[bbindex * BoxTensorLabel + 2];
                    h  = detectData[bbindex * BoxTensorLabel + 3];
                    cf = detectData[bbindex * BoxTensorLabel + 4];
                    //cout<<setprecision(10)<<x<<" "<<y<<" "<<w<<" "<<h<<" "<<cf<<endl;
                    float MaxClass =0.0f;
                    uint32_t MaxClass_Loc = 0;
                    for (int j = 5;j< BoxTensorLabel; j++)
                    {
                        float class_prob = detectData[bbindex * BoxTensorLabel + j];;
                        if(MaxClass < class_prob)
                        {
                            MaxClass = class_prob;
                            MaxClass_Loc = j - 5;
                        }
                    }


                    if((cf * MaxClass >= MaxBoxClassThresh))
                    {   BBox boundBox;
                        boundBox.rect.ltX = max((uint32_t)((x - w/2.0 - dw) / Scale), (uint32_t)0);
                        boundBox.rect.ltY = max((uint32_t)((y - h/2.0 - dh) / Scale), (uint32_t)0);
                        boundBox.rect.rbX = min((uint32_t)((x + w/2.0 - dw) / Scale), W);
                        boundBox.rect.rbY = min((uint32_t)((y + h/2.0 - dh) / Scale), H);
                        boundBox.score = cf * MaxClass;
                        boundBox.cls = MaxClass_Loc;
                        binfo.push_back(boundBox);
                    }
                }
            }
        }
        if (runMode_ == ACL_HOST) {
            delete[] detectData;
        }
    }
    /*
    for(auto &item : binfo){
        cout<<setprecision(10)<<item.rect.ltX<<" "<<item.rect.ltY<<" "<<item.rect.rbX<<" "<<item.rect.rbY<<" "<<item.score<<endl;
    }
    */
    //NMS
    bboxesNew = Utils::nmsAllClasses(nmsThresh, binfo, numClasses);
    return bboxesNew;
}


void* ObjectDetect::GetInferenceOutputItem(uint32_t& itemDataSize, aclmdlDataset* inferenceOutput, uint32_t idx) {
    aclDataBuffer* dataBuffer = aclmdlGetDatasetBuffer(inferenceOutput, idx);
    if (dataBuffer == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer from model "
        "inference output failed", idx);
        return nullptr;
    }

    void* dataBufferDev = aclGetDataBufferAddr(dataBuffer);
    if (dataBufferDev == nullptr) {
        ERROR_LOG("Get the %dth dataset buffer address "
        "from model inference output failed", idx);
        return nullptr;
    }

    size_t bufferSize = aclGetDataBufferSize(dataBuffer);
    if (bufferSize == 0) {
        ERROR_LOG("The %dth dataset buffer size of "
        "model inference output is 0", idx);
        return nullptr;
    }

    void* data = nullptr;
    if (runMode_ == ACL_HOST) {
        data = Utils::CopyDataDeviceToLocal(dataBufferDev, bufferSize);
        if (data == nullptr) {
            ERROR_LOG("Copy inference output to host failed");
            return nullptr;
        }
    }
    else {
        data = dataBufferDev;
    }

    itemDataSize = bufferSize;
    return data;
}

void ObjectDetect::DrawBoundBoxToImage(vector<BBox>& detectionResults, const string& origImagePath) {
    cv::Mat image = cv::imread(origImagePath, cv::IMREAD_UNCHANGED);
    for (int i = 0; i < detectionResults.size(); ++i) {
        cv::Point p1, p2;
        p1.x = detectionResults[i].rect.ltX;
        p1.y = detectionResults[i].rect.ltY;
        p2.x = detectionResults[i].rect.rbX;
        p2.y = detectionResults[i].rect.rbY;
        cv::rectangle(image, p1, p2, kColors[i % kColors.size()], kLineSolid);
        string text = yolov3Label[detectionResults[i].cls];
        cv::putText(image, text, cv::Point(p1.x, p1.y + kLabelOffset),
        cv::FONT_HERSHEY_COMPLEX, kFountScale, kFontColor);
    }

    int pos = origImagePath.find_last_of("/");
    string filename(origImagePath.substr(pos + 1));
    stringstream sstream;
    sstream.str("");
    sstream << "./output/out_" << filename;
    cv::imwrite(sstream.str(), image);
}

void ObjectDetect::WriteBoundBoxToTXT(vector<BBox>& detectionResults, const string& origImagePath) {
    int pos = origImagePath.find_last_of("/");
    string filename(origImagePath.substr(pos + 1));
    pos = filename.find_last_of(".");
    filename = filename.substr(0, pos);
    ofstream out;
    out.open(filename + ".txt", ofstream::app);
    for (int i = 0; i < detectionResults.size(); ++i) {
        int x1, y1, x2, y2;
        x1 = detectionResults[i].rect.ltX;
        y1 = detectionResults[i].rect.ltY;
        x2 = detectionResults[i].rect.rbX;
        y2 = detectionResults[i].rect.rbY;
        string text = yolov3Label[detectionResults[i].cls];
        float score = detectionResults[i].score;
        out<<text<<" "<<score<<" "<<x1<<" "<<y1<<" "<<x2<<" "<<y2<<endl;
    }
    out.close();
}


void ObjectDetect::DestroyResource()
{
    aclrtFree(imageDataBuf_);
    model_.DestroyResource();
	aclError ret;
    ret = aclrtResetDevice(deviceId_);
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("reset device failed");
    }
    INFO_LOG("end to reset device is %d", deviceId_);

    ret = aclFinalize();
    if (ret != ACL_ERROR_NONE) {
        ERROR_LOG("finalize acl failed");
    }
    INFO_LOG("end to finalize acl");
}

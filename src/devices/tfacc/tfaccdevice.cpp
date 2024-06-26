//
// Created by huangyuyang on 11/4/24.
//

#include <sys/mman.h>
#include <fcntl.h>

#include "devices/tfacc/tfaccdevice.h"
#include "devices/tfacc/fastllm-tfacc.h"

#include <cstring>
#include <thread>

#include <cfloat>
#include <cmath>

#ifdef __aarch64__
#include <arm_neon.h>
#include "armMath.h"
#endif

#include "utils.h"

namespace fastllm {
    void GetArrayMinMax(float *a, int len, float &minValue, float &maxValue) {
        int j = 0;
        minValue = 1e100;
        maxValue = -1e100;
#ifdef __aarch64__
        float32x4_t mins = vdupq_n_f32(1e100);
        float32x4_t maxs = vdupq_n_f32(-1e100);
        for (; j + 3 < len; j += 4) {
            float32x4_t v = vld1q_f32(a + j);
            mins = vminq_f32(mins, v);
            maxs = vmaxq_f32(maxs, v);
        }
        for (int l = 0; l < 4; l++) {
            minValue = std::min(minValue, mins[l]);
            maxValue = std::max(maxValue, maxs[l]);
        }
#endif
        for (; j < len; j++) {
            minValue = std::min(minValue, a[j]);
            maxValue = std::max(maxValue, a[j]);
        }
    }

    void QuantizationAll(float *fValue, uint8_t *uValue, int len, LowBitConfig *config) {
        float scale = config->scale;
        float zeroPoint = config->zeroPoint;
        int j = 0;
#ifdef __aarch64__
        float32x4_t scales = vdupq_n_f32(scale);
        float32x4_t zeros = vdupq_n_f32(zeroPoint + 0.5);
        int32x4_t maxds = vcombine_s32(vcreate_s32(0x000000ff000000ff), vcreate_s32(0x000000ff000000ff));
        int32x4_t minds = vcombine_s32(vcreate_s32(0x0000000000000000), vcreate_s32(0x0000000000000000));
        for (; j + 7 < len; j += 8) {
            float32x4_t fin1 = vld1q_f32(fValue + j);
            float32x4_t fin2 = vld1q_f32(fValue + j + 4);
            fin1 = vaddq_f32(vdivq_f32(fin1, scales), zeros);
            fin2 = vaddq_f32(vdivq_f32(fin2, scales), zeros);
            int32x4_t out1 = vcvtq_s32_f32(fin1);
            int32x4_t out2 = vcvtq_s32_f32(fin2);
            out1 = vmaxq_s32(out1, minds);
            out1 = vminq_s32(out1, maxds);
            out2 = vmaxq_s32(out2, minds);
            out2 = vminq_s32(out2, maxds);
            uint16x8_t out3 = vpaddq_u16(vreinterpretq_u16_s32(out1), vreinterpretq_u16_s32(out2));
            uint8x8_t out = vmovn_u16(out3);
            vst1_u8(uValue + j, out);
        }
#endif
        for (; j < len; j++) {
            uValue[j] = (uint8_t) (std::min(255., (double) std::max(fValue[j] / scale + zeroPoint + 0.5, 0.0)));
        }
    }

    static TfaccClient tfaccClient;

    TfaccDevice::TfaccDevice() {
        this->deviceType = "tfacc";
        this->ops["Linear"] = (BaseOperator *) (new TfaccLinearOp());
        this->ops["CatDirect"] = (BaseOperator *) (new TfaccCatDirectOp());

        this->ops["Attention"] = (BaseOperator *) (new TfaccAttention());
    }

    bool TfaccDevice::Malloc(void **ret, size_t size) {
        *ret = (void*)new uint8_t [size];
        return true;
    }

    bool TfaccDevice::Free(void *ret) {
        delete[] (uint8_t *)ret;
        return true;
    }

    bool TfaccDevice::CopyDataToCPU(void *dst, void *src, size_t size) {
        return true;
    }
    
    bool TfaccDevice::CopyDataFromCPU(void *dst, void *src, size_t size) {
        return true;
    }

    bool TfaccLinearOp::CanRun(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        if (datas.find("weight") == datas.end()) {
            return true;
        }
        Data *input = (datas.find("input")->second);
        Data *weight = (datas.find("weight")->second);
        return weight == nullptr ||
                weight->dataType == DataType::INT4_NOZERO ||
                weight->dataType == DataType::INT8 ||
                weight->dataType == DataType::INT4_GROUP ||
                weight->dataType == DataType::FLOAT32 ||
                weight->dataType == DataType::FLOAT16;
    }

    void TfaccLinearOp::Reshape(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);

        AssertInFastLLM(weight.dims.size() == 2, "Linear's weight's shape's size should be 2.\n");
        AssertInFastLLM(input.dims.back() == weight.dims[1], "Linear's weight's shape error.\n");

        weight.weightType = WeightType::LINEAR;
        std::vector <int> dims = input.dims;
        dims.back() = weight.dims[0];

        if (intParams.find("exType") != intParams.end()) {
            LinearExType type = (LinearExType)intParams.find("exType")->second;
            if (type == LinearExType::ExSwiglu) {
                dims.back() /= 2;
            }
        }

        output.dataType = input.dataType;
        output.Resize(dims);
    }

    void TfaccLinearOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
//auto st = std::chrono::system_clock::now();
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);
        Data &bias = *(datas.find("bias")->second);

        output.Allocate(0.0f);
        int n = input.Count(0) / input.dims.back();
        int m = input.dims.back();
        int k = output.dims.back();
        LinearExType exType = LinearExType::ExTypeNone;
        if (intParams.find("exType") != intParams.end()) {
            exType = (LinearExType)intParams.find("exType")->second;
            if (exType == LinearExType::ExSwiglu) {
                k *= 2;
            }
        }

        if (input.dataType == DataType::FLOAT32 && output.dataType == DataType::FLOAT32) {
            if (weight.dataType == DataType::FLOAT32 || weight.dataType == DataType::FLOAT16) {
                tfaccClient.RunTfaccLinearF(n, m, k, &weight, &bias, (float*)input.cpuData, (float*)output.cpuData, exType, input.dataType);
/*
                std::vector <float> tempOutputs;
                for (int i = 0; i < output.Count(0); i++) {
                    tempOutputs.push_back(((float*)output.cpuData)[i]);
                }
                CpuLinearOp::Run(opType, datas, floatParams, intParams);
                for (int i = 0; i < output.Count(0); i++) {
                    if (fabs(((float*)output.cpuData)[i] - tempOutputs[i]) > 1e-5) {
                        printf("wrong %d %f %f.\n", i, ((float*)output.cpuData)[i], tempOutputs[i]);
                        exit(0);
                    }
                }
*/
            } else if (weight.dataType == DataType::INT4 || 
                    weight.dataType == DataType::INT4_NOZERO ||
                    weight.dataType == DataType::INT4_GROUP ||
                    weight.dataType == DataType::INT8) {
                float *inputData = (float *) input.cpuData;
                uint8_t *weightData = (uint8_t *) weight.cpuData;
                float *outputData = (float *) output.cpuData;
                float *biasData = bias.dims.size() > 0 ? (float *) bias.cpuData : nullptr;
                weight.CalcWeightSum();

                int group = weight.group, groupCnt = weight.groupCnt;
                if (weight.dataType != DataType::INT4_GROUP) {
                    group = 1;
                    groupCnt = m;
                }

                std::vector<LowBitConfig> inputConfigs;
                for (int i = 0; i < n; i++) {
                    for (int g = 0; g < group; g++) {
                        int st = g * groupCnt;
                        int end = std::min(m, (g + 1) * groupCnt);
                        float minValue = 1e9, maxValue = -1e9;
                        GetArrayMinMax(inputData + i * m + st, end - st, minValue, maxValue);
                        inputConfigs.push_back(LowBitConfig(minValue, maxValue, 8, 0));
                    }
                }

                std::vector<uint8_t> uinput;
                uinput.resize(n * m);
                for (int i = 0; i < n; i++) {
                    float *cur = inputData + i * m;
                    uint8_t *u = uinput.data() + i * m;
                    for (int g = 0; g < group; g++) {
                        int st = g * groupCnt;
                        int end = std::min(m, (g + 1) * groupCnt);
                        QuantizationAll(cur + st, u + st, end - st, &inputConfigs[i * group + g]);
                    }
                }

                if (weight.dataType == DataType::INT4) {
                    ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
                } else if (weight.dataType == DataType::INT8 || weight.dataType == DataType::INT4_NOZERO || weight.dataType == DataType::INT4_GROUP) {
                    tfaccClient.RunTfaccLinearU(n, m, k, group, groupCnt, &weight, &bias, &inputConfigs, uinput.data(), outputData, exType);
                }
            }
        } else if (input.dataType == DataType::FLOAT16 && output.dataType == DataType::FLOAT16) {
            ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
        } else {
            ErrorInFastLLM("Linear error: unsupport weight's dataType.\n");
        }
//float spend = GetSpan(st, std::chrono::system_clock::now());
//float gops = (float)n * m * k / spend / 1e9;
// printf("n = %d, m = %d, k = %d, spend %f s, gops = %f\n", n, m, k, spend, gops);
    }

    long long int TfaccLinearOp::Ops(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &input = *(datas.find("input")->second);
        Data &output = *(datas.find("output")->second);
        Data &weight = *(datas.find("weight")->second);

        int n = input.Count(0) / input.dims.back();
        int m = input.dims.back();
        int k = output.dims.back();

        return (long long int) n * m * k;
    }

    void TfaccCatDirectOp::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        CpuCatDirectOp::Run(opType, datas, floatParams, intParams);

        // 如果是kvCache，那么要同步到server上
        Data *input0 = (datas.find("input0")->second);
        Data *input1 = (datas.find("input1")->second);

        if (input0->isKVCache) {
            tfaccClient.AppendKVCache(input0->cacheUid, input1);
        }
    }

    void TfaccAttention::Run(const std::string &opType, const DataDict &datas, const FloatDict &floatParams, const IntDict &intParams) {
        Data &q = *(datas.find("q")->second);
        Data &k = *(datas.find("k")->second);
        Data &v = *(datas.find("v")->second);
        int maskType = intParams.find("maskType") != intParams.end() ? intParams.find("maskType")->second : 0;

        if (!k.isKVCache || !v.isKVCache || maskType != 0) {
            CpuAttention::Run(opType, datas, floatParams, intParams);
            return;
        }

        int group = intParams.find("group") != intParams.end() ? intParams.find("group")->second : 1;
        float scale = floatParams.find("scale") != floatParams.end() ? floatParams.find("scale")->second : 1.0;

        Data &output = *(datas.find("output")->second);
        output.Allocate();

        tfaccClient.Attention(&q, &k, &v, group, scale, maskType, &output);
/*
        std::vector <float> tempOutput;
        for (int i = 0; i < output.Count(0); i++) {
            tempOutput.push_back(((float*)output.cpuData)[i]);
        }

        CpuAttention::Run(opType, datas, floatParams, intParams);
        for (int i = 0; i < output.Count(0); i++) {
            float x = ((float*)output.cpuData)[i];
            if (fabs(x - tempOutput[i]) > 1e-5) {
                printf("wrong %d %f %f\n", i, x, tempOutput[i]);
                exit(0);
            }
        }
*/
    }
}
/*
*	Copyright (c) 2018, Ilya Kalinovskiy
*	All rights reserved.
*
*	This is an implementation of the algorithm described in the following paper:
*		I.A. Kalinovskiy, V.G. Spitsyn,
*		Compact Convolutional Neural Network Cascade for Face Detection,
*		http://arxiv.org/abs/1508.01292.
*
*	Redistribution and use of this program as source code or in binary form, with or without modifications, are permitted provided that the following conditions are met:
*		1. Redistributions may not be sold, nor may they be used in a commercial product or activity without prior permission from the copyright holder (contact him at kua_21@mail.ru).
*		2. Redistributions may not be used for military purposes.
*		3. Any published work which utilizes this program shall include the reference to the paper available at http://arxiv.org/abs/1508.01292
*		4. Redistributions must retain the above copyright notice and the reference to the algorithm on which the implementation is based on, this list of conditions and the following disclaimer.
*
*	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
*	IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/


#include "cnnpp_cl.h"
#include <fstream>
#include <sstream>
#ifdef USE_RESOURCE
#	include <windows.h>
#endif


//================================================================================================================================================


namespace NeuralNetworksLib
{
#if defined(USE_CL) && !defined(USE_CNTK_MODELS)

	namespace CL
	{
		#define surf_L1 4
		#define surf_L2 8
		#define surf_L3 16
		#define surf_hl 32
		#define kernel_size_L1 4*4
		#define kernel_size_L2 3*3
		#define kernel_size_L3 6*5

		cl_device_type device_type;
		cl_program CNNPPProgram = 0;

		//kernels
		//L1
		cl_kernel conv_4x4x4_max_tanh_tanh_cl = 0;
		
		//L2
		cl_kernel add_2_3_conv_4x3x3_max_tanh_tanh_L_cl = 0;
		cl_kernel add_2_3_conv_4x3x3_max_tanh_tanh_R_cl = 0;
		
		//L3
		cl_kernel add_2_3_conv_4x6x5_L_cl = 0;
		cl_kernel add_3_conv_4x6x5_1_cl = 0;
		cl_kernel add_3_conv_4x6x5_2_cl = 0;
		cl_kernel add_2_3_conv_4x6x5_R_cl = 0;
		
		//HL
		cl_kernel add_16_tanh_tanh_2tanh_tanh_texmem_cl = 0;

		//cnn weight
		Image_32f kernels_L1[surf_L1];
		Image_32f kernels_L2[surf_L2];
		Image_32f kernels_L3[surf_L3];

		Image_32f conv_b_L1;
		Image_32f conv_b_L2;
		Image_32f conv_b_L3;

		Image_32f subs_w_L1;
		Image_32f subs_w_L2;
		Image_32f subs_w_L3;

		Image_32f subs_b_L1;
		Image_32f subs_b_L2;
		Image_32f subs_b_L3;

		Image_32f scale_L1;
		Image_32f scale_L2;
		Image_32f scale_L3;
		Image_32f scale_HL;

		Image_32f hl_w;
		Image_32f hl_b;

		Image_32f ol_w;
		Image_32f ol_b;

		//---------------------------------------------------------------------------------------------------------------------------------------------------------------

		void CNNPP::create_cl_program(cl_device_id _device, cl_context _context)
		{
#ifdef USE_RESOURCE
			HMODULE module = NULL;
			GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&CNNPP::create_cl_program),
				&module);
			HRSRC resource = FindResource(module, MAKEINTRESOURCE(CNNPP_CL_KERNELS), RT_RCDATA);
			if (resource == 0)
			{
				printf("Resource CNNPP_CL_KERNELS not found!\n");
				return;
			}

			HGLOBAL resourceData = LoadResource(module, resource);
			void* pBinaryData = LockResource(resourceData);
			unsigned int resourceSize = SizeofResource(module, resource);

			std::string str_source_code;
			str_source_code.append((char*)pBinaryData, resourceSize);
			const char* c_source_code[1] = { str_source_code.c_str() };
#else
			std::ifstream source_file(SOURCE"cnnpp_cl_kernels.cl");
			std::string str_source_code(std::istreambuf_iterator<char>(source_file), (std::istreambuf_iterator<char>()));
			const char* c_source_code[1] = { str_source_code.c_str() };
#endif

			clERR(clGetDeviceInfo(_device, CL_DEVICE_TYPE, sizeof(device_type), &device_type, NULL));

			cl_int _err;
			CNNPPProgram = clCreateProgramWithSource(_context, sizeof(c_source_code) / sizeof(*c_source_code), c_source_code, NULL, &_err);
			char options[] = "-cl-unsafe-math-optimizations -cl-mad-enable";
			if (clBuildProgram(CNNPPProgram, 1, &_device, options, NULL, NULL) != CL_SUCCESS)
			{
				char buffer[10240];
				clGetProgramBuildInfo(CNNPPProgram, _device, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, NULL);
				fprintf(stderr, "CL Compilation failed:\n%s", buffer);
				abort();
			}
			clUnloadCompiler();

			//create kernels
			conv_4x4x4_max_tanh_tanh_cl = clCreateKernel(CNNPPProgram, "conv_4x4x4_max_tanh_tanh_cl", &_err);
			clERR(_err);

			add_2_3_conv_4x3x3_max_tanh_tanh_L_cl = clCreateKernel(CNNPPProgram, "add_2_3_conv_4x3x3_max_tanh_tanh_L_cl", &_err);
			clERR(_err);

			add_2_3_conv_4x3x3_max_tanh_tanh_R_cl = clCreateKernel(CNNPPProgram, "add_2_3_conv_4x3x3_max_tanh_tanh_R_cl", &_err);
			clERR(_err);

			add_2_3_conv_4x6x5_L_cl = clCreateKernel(CNNPPProgram, "add_2_3_conv_4x6x5_L_cl", &_err);
			clERR(_err);

			add_3_conv_4x6x5_1_cl = clCreateKernel(CNNPPProgram, "add_3_conv_4x6x5_1_cl", &_err);
			clERR(_err);

			add_3_conv_4x6x5_2_cl = clCreateKernel(CNNPPProgram, "add_3_conv_4x6x5_1_cl", &_err);
			clERR(_err);

			add_2_3_conv_4x6x5_R_cl = clCreateKernel(CNNPPProgram, "add_2_3_conv_4x6x5_L_cl", &_err);
			clERR(_err);

			add_16_tanh_tanh_2tanh_tanh_texmem_cl = clCreateKernel(CNNPPProgram, "add_16_tanh_tanh_2tanh_tanh_texmem_cl", &_err);
			clERR(_err);
		}
		void CNNPP::destroy_cl_program()
		{
			if (CNNPPProgram != 0)
			{
				clERR(clReleaseKernel(conv_4x4x4_max_tanh_tanh_cl));
				clERR(clReleaseKernel(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl));
				clERR(clReleaseKernel(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl));
				clERR(clReleaseKernel(add_2_3_conv_4x6x5_L_cl));
				clERR(clReleaseKernel(add_3_conv_4x6x5_1_cl));
				clERR(clReleaseKernel(add_3_conv_4x6x5_2_cl));
				clERR(clReleaseKernel(add_2_3_conv_4x6x5_R_cl));
				clERR(clReleaseKernel(add_16_tanh_tanh_2tanh_tanh_texmem_cl));
				clERR(clReleaseProgram(CNNPPProgram));
			}
		}

		//---------------------------------------------------------------------------------------------------------------------------------------------------------------

		void CNNPP::set_src_surf(Image_32f* src, int layer, int surface)
		{
			switch (layer)
			{
			case 0:	//Input
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 5, sizeof(src->dataDevice), &src->dataDevice));
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 6, sizeof(src->width), &src->width));
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 7, sizeof(src->height), &src->height));
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 8, sizeof(src->widthStepDevice), &src->widthStepDevice));
				break;

			case 1:	//L1
				if (surface < 3)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 5 + surface, sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 0)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 8, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 9, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 10, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				if (surface > 0)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 5 + (surface - 1), sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 3)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 8, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 9, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 10, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				break;

			case 2:	//L2
				if (surface < 3)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 5 + surface, sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 0)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 8, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 9, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 10, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				if (surface > 0 && surface < 5)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 5 + (surface - 1), sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 4)
					{
						clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 9, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 10, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 11, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				if (surface > 2 && surface < 7)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, (5 + 4 - 1) - (surface - 3), sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 3)
					{
						clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 9, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 10, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 11, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				if (surface > 4)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, (5 + 3 - 1) - (surface - 5), sizeof(src->dataDevice), &src->dataDevice));
					if (surface == 7)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 8, sizeof(src->width), &src->width));
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 9, sizeof(src->height), &src->height));
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 10, sizeof(src->widthStepDevice), &src->widthStepDevice));
					}
				}
				break;

			case 3:	//L3
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 4 + surface, sizeof(src->dataDevice), &src->dataDevice));
				if (surface == 0)
				{
					clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 20, sizeof(src->width), &src->width));
					clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 21, sizeof(src->height), &src->height));
					clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 22, sizeof(src->widthStepDevice), &src->widthStepDevice));
				}
				break;

			case 4:	//HL
				break;

			default:
				system("pause");
			}
		}
		void CNNPP::set_dst_surf(Image_32f* dst, int layer, int surface)
		{
			switch (layer)
			{
			case 1:	//L1
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, surface, sizeof(dst->dataDevice), &dst->dataDevice));
				if (surface == 0)
				{
					clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
				}
				break;

			case 2:	//L2
				if (surface < 4)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, surface, sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 0)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				else
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, (surface - 4), sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 7)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				break;

			case 3:	//L3
				if (surface < 4)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, surface, sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 0)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				if (surface >= 4 && surface < 8)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, (surface - 4), sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 4)
					{
						clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				if (surface >= 8 && surface < 12)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 3 - (surface - 8), sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 11)
					{
						clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				if (surface >= 12)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 3 - (surface - 12), sizeof(dst->dataDevice), &dst->dataDevice));
					if (surface == 15)
					{
						clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 4, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
					}
				}
				break;

			case 4:	//HL
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 0, sizeof(dst->dataDevice), &dst->dataDevice));
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 1, sizeof(dst->width), &dst->width));
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 2, sizeof(dst->height), &dst->height));
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 3, sizeof(dst->widthStepDevice), &dst->widthStepDevice));
				break;

			default:
				system("pause");
			}
		}

		//---------------------------------------------------------------------------------------------------------------------------------------------------------------

		void CNNPP::set_kernel_on_device(cl_context _context, cl_command_queue _queue, float* kernel, int size, int layer, int surface)
		{
			switch (layer)
			{
			case 1:
				kernels_L1[surface] = Image_32f(_context, _queue, size, 1, 1, kernel);
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 9 + surface, sizeof(kernels_L1[surface].dataDevice), &kernels_L1[surface].dataDevice));
				break;

			case 2:
				kernels_L2[surface] = Image_32f(_context, _queue, size, 1, 1, kernel);
				if (surface < 4)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 11 + surface, sizeof(kernels_L2[surface].dataDevice), &kernels_L2[surface].dataDevice));
				}
				else
				{
					clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 11 + (surface - 4), sizeof(kernels_L2[surface].dataDevice), &kernels_L2[surface].dataDevice));
				}
				break;

			case 3:
				kernels_L3[surface] = Image_32f(_context, _queue, size, 1, 1, kernel);
				if (surface < 4)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_L_cl, 11 + surface, sizeof(kernels_L3[surface].dataDevice), &kernels_L3[surface].dataDevice));
				}
				if (surface >= 4 && surface < 8)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_1_cl, 12 + (surface - 4), sizeof(kernels_L3[surface].dataDevice), &kernels_L3[surface].dataDevice));
				}
				if (surface >= 8 && surface < 12)
				{
					clERR(clSetKernelArg(add_3_conv_4x6x5_2_cl, 15 - (surface - 8), sizeof(kernels_L3[surface].dataDevice), &kernels_L3[surface].dataDevice));
				}
				if (surface >= 12)
				{
					clERR(clSetKernelArg(add_2_3_conv_4x6x5_R_cl, 14 - (surface - 12), sizeof(kernels_L3[surface].dataDevice), &kernels_L3[surface].dataDevice));
				}
				break;
			}
		}
		void CNNPP::set_conv_b_on_device(cl_context _context, cl_command_queue _queue, float* conv_b, int layer, int surface)
		{
			switch (layer)
			{
			case 1:
				conv_b_L1 = Image_32f(_context, _queue, surface, 1, 1, conv_b);
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 13, sizeof(conv_b_L1.dataDevice), &conv_b_L1.dataDevice));
				break;

			case 2:
				conv_b_L2 = Image_32f(_context, _queue, surface, 1, 1, conv_b);
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 15, sizeof(conv_b_L2.dataDevice), &conv_b_L2.dataDevice));
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 15, sizeof(conv_b_L2.dataDevice), &conv_b_L2.dataDevice));
				break;

			case 3:
				conv_b_L3 = Image_32f(_context, _queue, surface, 1, 1, conv_b);
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 23, sizeof(conv_b_L3.dataDevice), &conv_b_L3.dataDevice));
				break;
			}
		}
		void CNNPP::set_subs_w_on_device(cl_context _context, cl_command_queue _queue, float* subs_w, int layer, int surface)
		{
			switch (layer)
			{
			case 1:
				subs_w_L1 = Image_32f(_context, _queue, surface, 1, 1, subs_w);
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 14, sizeof(subs_w_L1.dataDevice), &subs_w_L1.dataDevice));
				break;

			case 2:
				subs_w_L2 = Image_32f(_context, _queue, surface, 1, 1, subs_w);
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 16, sizeof(subs_w_L2.dataDevice), &subs_w_L2.dataDevice));
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 16, sizeof(subs_w_L2.dataDevice), &subs_w_L2.dataDevice));
				break;

			case 3:
				subs_w_L3 = Image_32f(_context, _queue, surface, 1, 1, subs_w);
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 24, sizeof(subs_w_L3.dataDevice), &subs_w_L3.dataDevice));
				break;
			}
		}
		void CNNPP::set_subs_b_on_device(cl_context _context, cl_command_queue _queue, float* subs_b, int layer, int surface)
		{
			switch (layer)
			{
			case 1:
				subs_b_L1 = Image_32f(_context, _queue, surface, 1, 1, subs_b);
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 15, sizeof(subs_b_L1.dataDevice), &subs_b_L1.dataDevice));
				break;

			case 2:
				subs_b_L2 = Image_32f(_context, _queue, surface, 1, 1, subs_b);
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 17, sizeof(subs_b_L2.dataDevice), &subs_b_L2.dataDevice));
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 17, sizeof(subs_b_L2.dataDevice), &subs_b_L2.dataDevice));
				break;

			case 3:
				subs_b_L3 = Image_32f(_context, _queue, surface, 1, 1, subs_b);
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 25, sizeof(subs_b_L3.dataDevice), &subs_b_L3.dataDevice));
				break;
			}
		}
		void CNNPP::set_scale_on_device(cl_context _context, cl_command_queue _queue, float* scale, int layer)
		{
			switch (layer)
			{
			case 1:
				scale_L1 = Image_32f(_context, _queue, 1, 1, 1, scale);
				clERR(clSetKernelArg(conv_4x4x4_max_tanh_tanh_cl, 16, sizeof(scale_L1.dataDevice), &scale_L1.dataDevice));
				break;

			case 2:
				scale_L2 = Image_32f(_context, _queue, 1, 1, 1, scale);
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 18, sizeof(scale_L2.dataDevice), &scale_L2.dataDevice));
				clERR(clSetKernelArg(add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 18, sizeof(scale_L2.dataDevice), &scale_L2.dataDevice));
				break;

			case 3:
				scale_L3 = Image_32f(_context, _queue, 1, 1, 1, scale);
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 26, sizeof(scale_L3.dataDevice), &scale_L3.dataDevice));
				break;

			case 4:
				scale_HL = Image_32f(_context, _queue, 1, 1, 1, scale);
				clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 31, sizeof(scale_HL.dataDevice), &scale_HL.dataDevice));
				break;
			}
		}
		void CNNPP::set_hl_w_on_device(cl_context _context, cl_command_queue _queue, float* _hl_w, int surface)
		{
			hl_w = Image_32f(_context, _queue, surface, 1, 1, _hl_w);
			clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 27, sizeof(hl_w.dataDevice), &hl_w.dataDevice));
		}
		void CNNPP::set_hl_b_on_device(cl_context _context, cl_command_queue _queue, float* _hl_b, int surface)
		{
			hl_b = Image_32f(_context, _queue, surface, 1, 1, _hl_b);
			clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 28, sizeof(hl_b.dataDevice), &hl_b.dataDevice));
		}
		void CNNPP::set_ol_w_on_device(cl_context _context, cl_command_queue _queue, float* _ol_w, int surface)
		{
			ol_w = Image_32f(_context, _queue, surface, 1, 1, _ol_w);
			clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 29, sizeof(ol_w.dataDevice), &ol_w.dataDevice));
		}
		void CNNPP::set_ol_b_on_device(cl_context _context, cl_command_queue _queue, float* _ol_b)
		{
			ol_b = Image_32f(_context, _queue, 1, 1, 1, _ol_b);
			clERR(clSetKernelArg(add_16_tanh_tanh_2tanh_tanh_texmem_cl, 30, sizeof(ol_b.dataDevice), &ol_b.dataDevice));
		}

		void CNNPP::release_cl_buffers()
		{
			for (int i = 0; i < surf_L1; ++i)
			{
				kernels_L1[i].clear();
			}
			for (int i = 0; i < surf_L2; ++i)
			{
				kernels_L2[i].clear();
			}
			for (int i = 0; i < surf_L3; ++i)
			{
				kernels_L3[i].clear();
			}

			conv_b_L1.clear();
			conv_b_L2.clear();
			conv_b_L3.clear();

			subs_w_L1.clear();
			subs_w_L2.clear();
			subs_w_L3.clear();

			subs_b_L1.clear();
			subs_b_L2.clear();
			subs_b_L3.clear();

			scale_L1.clear();
			scale_L2.clear();
			scale_L3.clear();
			scale_HL.clear();
			 
			hl_w.clear();
			hl_b.clear();

			ol_w.clear();
			ol_b.clear();
		}

		//---------------------------------------------------------------------------------------------------------------------------------------------------------------

		void CNNPP::run_L1_max(Size2d ROI, Size block_size, cl_command_queue _queue)
		{
			if (device_type == CL_DEVICE_TYPE_CPU)
			{
				size_t global_work_size[2] = { roundUp(ROI.cols, 2), roundUp(ROI.rows, 2) };
				clERR(clEnqueueNDRangeKernel(_queue, conv_4x4x4_max_tanh_tanh_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
			}
			else
			{
				size_t global_work_size[2] = { roundUpMul(roundUp(ROI.cols, 2), block_size.width >> 1), roundUpMul(roundUp(ROI.rows, 2), block_size.height >> 1) };
				size_t local_work_size[2] = { block_size.width >> 1, block_size.height >> 1 };
				clERR(clEnqueueNDRangeKernel(_queue, conv_4x4x4_max_tanh_tanh_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
			}
		}
		void CNNPP::run_L2_max(Size2d ROI, Size block_size, cl_command_queue _queue)
		{
			if (device_type == CL_DEVICE_TYPE_CPU)
			{
				size_t global_work_size[2] = { roundUp(ROI.cols, 2), roundUp(ROI.rows, 2) };	
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
			}
			else
			{
				size_t global_work_size[2] = { roundUpMul(roundUp(ROI.cols, 2), block_size.width >> 1), roundUpMul(roundUp(ROI.rows, 2), block_size.height >> 1) };
				size_t local_work_size[2] = { block_size.width >> 1, block_size.height >> 1 };
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x3x3_max_tanh_tanh_L_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x3x3_max_tanh_tanh_R_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
			}
		}
		void CNNPP::run_L3_max(Size2d ROI, Size block_size, cl_command_queue _queue)
		{
			if (device_type == CL_DEVICE_TYPE_CPU)
			{
				size_t global_work_size[2] = { roundUp(ROI.cols, 2), roundUp(ROI.rows, 2) };
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x6x5_L_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_3_conv_4x6x5_1_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_3_conv_4x6x5_2_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x6x5_R_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
			}
			else
			{
				size_t global_work_size[2] = { roundUpMul(roundUp(ROI.cols, 2), block_size.width >> 1), roundUpMul(roundUp(ROI.rows, 2), block_size.height >> 1) };
				size_t local_work_size[2] = { block_size.width >> 1, block_size.height >> 1 };
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x6x5_L_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_3_conv_4x6x5_1_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_3_conv_4x6x5_2_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
				clERR(clEnqueueNDRangeKernel(_queue, add_2_3_conv_4x6x5_R_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
			}
		}
		void CNNPP::run_HL_max(Size2d ROI, Size block_size, cl_command_queue _queue)
		{
			if (device_type == CL_DEVICE_TYPE_CPU)
			{
				size_t global_work_size[2] = { ROI.cols, ROI.rows };
				clERR(clEnqueueNDRangeKernel(_queue, add_16_tanh_tanh_2tanh_tanh_texmem_cl, 2, NULL, global_work_size, NULL, 0, NULL, NULL));
			}
			else
			{
				size_t global_work_size[2] = { roundUpMul(ROI.cols, block_size.width >> 1), roundUpMul(ROI.rows, block_size.height >> 1) };
				size_t local_work_size[2] = { block_size.width >> 1, block_size.height >> 1 };
				clERR(clEnqueueNDRangeKernel(_queue, add_16_tanh_tanh_2tanh_tanh_texmem_cl, 2, NULL, global_work_size, local_work_size, 0, NULL, NULL));
			}
		}
	}

#endif
}
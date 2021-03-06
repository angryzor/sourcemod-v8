#include "SPAPIEmulation.h"
#include <cmath>
#include <cstring>

namespace SMV8
{
	namespace SPEmulation
	{
		using namespace SourcePawn;

		PluginFunction::PluginFunction(PluginRuntime& runtime, funcid_t id) : runtime(runtime), id(id), curParam(0)
		{
		}

		PluginFunction::~PluginFunction()
		{
			Cancel();
		}

		int PluginFunction::PushValue(Local<Value> val)
		{
			params[curParam++].Reset(runtime.isolate, val);
			return SP_ERROR_NONE;
		}

		int PluginFunction::PushCell(cell_t cell)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(Integer::New(runtime.isolate, cell));
		}

		int PluginFunction::PushCellByRef(cell_t *cell, int flags)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(MakeRefObj(Integer::New(runtime.isolate, *cell), cell, 1, flags & SM_PARAM_COPYBACK));
		}

		int PluginFunction::PushFloat(float number)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(Number::New(runtime.isolate, number));
		}

		int PluginFunction::PushFloatByRef(float *number, int flags)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(MakeRefObj(Number::New(runtime.isolate, *number), number, 1, flags & SM_PARAM_COPYBACK));
		}

		// TODO: Problem: No float arrays
		int PluginFunction::PushArray(cell_t *inarray, unsigned int cells, int flags)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			Local<Array> array = Array::New(runtime.isolate, cells);

			for(unsigned int i = 0; i < cells; i++)
			{
				array->Set(i, Integer::New(runtime.isolate, inarray[i]));
			}

			return PushValue(MakeRefObj(array, inarray, cells, flags & SM_PARAM_COPYBACK));
		}

		int PluginFunction::PushString(const char *string)
		{
			// Alternative: give raw string back, but unpredictable
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(MakeRefObj(String::NewFromUtf8(runtime.isolate, string), NULL, strlen(string) + 1, false));
		}

		int PluginFunction::PushStringEx(char *buffer, size_t length, int sz_flags, int cp_flags)
		{
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);
			return PushValue(MakeRefObj(String::NewFromUtf8(runtime.isolate, buffer), buffer, length, cp_flags & SM_PARAM_COPYBACK));
		}

		Local<Object> PluginFunction::MakeRefObj(Local<Value> val, void *addr, size_t size, bool copyback)
		{
			EscapableHandleScope handle_scope(runtime.isolate);
			Local<Object> res = Object::New(runtime.isolate);
			res->Set(String::NewFromUtf8(runtime.isolate, "value"), val);
			res->Set(String::NewFromUtf8(runtime.isolate, "size"), Integer::New(runtime.isolate, size));

			if(copyback)
			{
				SPToV8ReferenceInfo *ri = new SPToV8ReferenceInfo;
				ri->refObj.Reset(runtime.isolate,res);
				ri->addr = addr;
				ri->size = size;
				refs.push_back(ri);
			}

			return handle_scope.Escape(res);
		}

		void PluginFunction::Cancel()
		{
			for(SPToV8ReferenceInfo *ref: refs)
			{
				ref->refObj.Reset();
				delete ref;
			}
			refs.clear();

			for(int i = 0; i < curParam; i++)
			{
				params[i].Reset();
			}
			curParam = 0;
		}

		int PluginFunction::Execute(cell_t *result)
		{
			return Execute2(runtime.GetDefaultContext(), result);
		}

		int PluginFunction::CallFunction(const cell_t *params, unsigned int num_params, cell_t *result)
		{
			return CallFunction2(runtime.GetDefaultContext(), params, num_params, result);
		}

		int PluginFunction::CallFunction2(IPluginContext *ctx, 
			const cell_t *params, 
			unsigned int num_params, 
			cell_t *result)
		{
			return runtime.GetDefaultContext()->Execute2(this, params, num_params, result);
		}

		IPluginContext *PluginFunction::GetParentContext()
		{
			return GetParentRuntime()->GetDefaultContext();
		}

		bool PluginFunction::IsRunnable()
		{
			return true;
		}

		funcid_t PluginFunction::GetFunctionID()
		{
			return id;
		}

		int PluginFunction::Execute2(IPluginContext *ctx, cell_t *result)
		{
			PluginContext* pcontext = static_cast<PluginContext*>(ctx);
			HandleScope handle_scope(runtime.isolate);
			Local<Context> context = Local<Context>::New(runtime.isolate, runtime.v8Context);
			Context::Scope context_scope(context);

			Local<Value> args[SP_MAX_EXEC_PARAMS];

			for(int i = 0; i < curParam; i++)
			{
				args[i] = Local<Value>::New(runtime.isolate, params[i]);
			}

			TryCatch trycatch;

			Local<Value> res = pcontext->ExecuteV8(this, curParam, args);

			if(res.IsEmpty()) 
			{  
				Local<Value> exception = trycatch.Exception();
				Local<Value> stack_trace = trycatch.StackTrace();
				String::Utf8Value exceptionStr(exception);

				std::string err = *exceptionStr;
				if(!stack_trace.IsEmpty())
				{
					err += "\n";
					err += *String::Utf8Value(stack_trace.As<String>());
				}

				runtime.manager->ReportError(ctx, GetFunctionID(), 0, err);

				Cancel();

				return SP_ERROR_ABORTED;
			}
			else
			{
				if(res->IsNumber())
					SetSingleCellValue(res.As<Number>(), result);
				CopyBackRefs();

				Cancel();

				return SP_ERROR_NONE;
			}
		}

		void PluginFunction::CopyBackRefs()
		{
			HandleScope handle_scope(runtime.isolate);

			for(SPToV8ReferenceInfo *ref: refs)
			{
				Local<Object> refObj = Local<Object>::New(runtime.isolate, ref->refObj);
				Local<Value> val = refObj->Get(String::NewFromUtf8(runtime.isolate, "value"));

				if(val->IsNumber())
					SetSingleCellValue(val.As<Number>(), (cell_t *)ref->addr);
				else if(val->IsString())
					CopyBackString(val.As<String>(), ref);
				else if(val->IsArray())
					CopyBackArray(val.As<Array>(), ref);
			}
		}

		void PluginFunction::SetSingleCellValue(Local<Number> val, cell_t *addr)
		{
			if(val->IsInt32())
				*addr = (cell_t)val.As<Integer>()->Value();
			else if(val->IsNumber())
				*addr = sp_ftoc((float)val->Value());
		}

		void PluginFunction::CopyBackString(Local<String> val, SPToV8ReferenceInfo *ref)
		{
			// TODO: UTF-8
			
			String::Utf8Value ascii(val);
			size_t len = std::min((size_t)ascii.length() + 1,ref->size);
			memcpy(ref->addr, *ascii, len);
			((char*)ref->addr)[len - 1] = '\0';
		}

		void PluginFunction::CopyBackArray(Local<Array> val, SPToV8ReferenceInfo *ref)
		{
			for(unsigned int i = 0; i < std::min(val->Length(), ref->size); i++)
			{
				SetSingleCellValue(val->Get(i).As<Number>(), &((cell_t *)ref->addr)[i]);
			}
		}

		IPluginRuntime *PluginFunction::GetParentRuntime()
		{
			return &runtime;
		}
	}
}

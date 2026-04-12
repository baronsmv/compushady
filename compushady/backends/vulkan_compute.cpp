#include "vulkan_common.h"

/* ----------------------------------------------------------------------------
   Compute Type
   ------------------------------------------------------------------------- */
static void vulkan_Compute_dealloc(vulkan_Compute *self) {
  if (self->py_device) {
    VkDevice device = self->py_device->device;
    if (self->pipeline)
      vkDestroyPipeline(device, self->pipeline, NULL);
    if (self->pipeline_layout)
      vkDestroyPipelineLayout(device, self->pipeline_layout, NULL);
    if (self->descriptor_pool)
      vkDestroyDescriptorPool(device, self->descriptor_pool, NULL);
    if (self->descriptor_set_layout)
      vkDestroyDescriptorSetLayout(device, self->descriptor_set_layout, NULL);
    if (self->shader_module)
      vkDestroyShaderModule(device, self->shader_module, NULL);
    if (self->dispatch_fence)
      vkDestroyFence(device, self->dispatch_fence, NULL);
    Py_DECREF(self->py_device);
  }
  Py_XDECREF(self->py_cbv_list);
  Py_XDECREF(self->py_srv_list);
  Py_XDECREF(self->py_uav_list);
  Py_XDECREF(self->py_samplers_list);
  Py_TYPE(self)->tp_free((PyObject *)self);
}

PyTypeObject vulkan_Compute_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "vulkan.Compute",
    .tp_basicsize = sizeof(vulkan_Compute),
    .tp_dealloc = (destructor)vulkan_Compute_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
};

/* ----------------------------------------------------------------------------
   Helper: submit and wait
   ------------------------------------------------------------------------- */
static VkResult submit_and_wait(vulkan_Device *dev, VkCommandBuffer cmd) {
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  VkResult res = vkQueueSubmit(dev->queue, 1, &submit, VK_NULL_HANDLE);
  if (res != VK_SUCCESS)
    return res;

  Py_BEGIN_ALLOW_THREADS;
  vkQueueWaitIdle(dev->queue);
  Py_END_ALLOW_THREADS;
  return VK_SUCCESS;
}

/* ----------------------------------------------------------------------------
   vulkan_Compute_dispatch
   ------------------------------------------------------------------------- */
PyObject *vulkan_Compute_dispatch(vulkan_Compute *self, PyObject *args) {
  uint32_t x, y, z;
  Py_buffer push = {0};
  if (!PyArg_ParseTuple(args, "III|y*", &x, &y, &z, &push))
    return NULL;

  if (push.len > 0) {
    if (push.len > self->push_constant_size || (push.len % 4) != 0) {
      PyBuffer_Release(&push);
      return PyErr_Format(PyExc_ValueError,
                          "Invalid push constant size: %u, expected max %u "
                          "with 4 bytes alignment",
                          (unsigned)push.len, self->push_constant_size);
    }
  }

  vulkan_Device *dev = self->py_device;
  VkCommandBuffer cmd = dev->command_buffer;

  VkCommandBufferBeginInfo begin = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &begin);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self->pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          self->pipeline_layout, 0, 1, &self->descriptor_set, 0,
                          NULL);

  if (push.len > 0) {
    vkCmdPushConstants(cmd, self->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, (uint32_t)push.len, push.buf);
  }

  vkCmdDispatch(cmd, x, y, z);
  vkEndCommandBuffer(cmd);

  if (push.buf)
    PyBuffer_Release(&push);

  VkResult res = submit_and_wait(dev, cmd);
  if (res != VK_SUCCESS)
    return PyErr_Format(PyExc_RuntimeError, "Dispatch submission failed: %d",
                        res);

  Py_RETURN_NONE;
}

/* ----------------------------------------------------------------------------
   vulkan_Compute_dispatch_indirect
   ------------------------------------------------------------------------- */
PyObject *vulkan_Compute_dispatch_indirect(vulkan_Compute *self,
                                           PyObject *args) {
  PyObject *indirect_obj;
  uint32_t offset;
  Py_buffer push = {0};
  if (!PyArg_ParseTuple(args, "OI|y*", &indirect_obj, &offset, &push))
    return NULL;

  if (!PyObject_TypeCheck(indirect_obj, &vulkan_Resource_Type)) {
    PyErr_SetString(PyExc_TypeError, "Expected a Buffer object");
    return NULL;
  }

  vulkan_Resource *indirect = (vulkan_Resource *)indirect_obj;
  if (!indirect->buffer) {
    PyErr_SetString(PyExc_TypeError, "Resource is not a buffer");
    return NULL;
  }

  if (push.len > 0) {
    if (push.len > self->push_constant_size || (push.len % 4) != 0) {
      PyBuffer_Release(&push);
      return PyErr_Format(PyExc_ValueError,
                          "Invalid push constant size: %u, expected max %u "
                          "with 4 bytes alignment",
                          (unsigned)push.len, self->push_constant_size);
    }
  }

  vulkan_Device *dev = self->py_device;
  VkCommandBuffer cmd = dev->command_buffer;

  VkCommandBufferBeginInfo begin = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cmd, &begin);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, self->pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          self->pipeline_layout, 0, 1, &self->descriptor_set, 0,
                          NULL);

  if (push.len > 0) {
    vkCmdPushConstants(cmd, self->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, (uint32_t)push.len, push.buf);
  }

  vkCmdDispatchIndirect(cmd, indirect->buffer, offset);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit_info = {};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.pCommandBuffers = &self->py_device->command_buffer;
  submit_info.commandBufferCount = 1;

  VkResult result = vkQueueSubmit(self->py_device->queue, 1, &submit_info,
                                  self->dispatch_fence);
  if (result != VK_SUCCESS) {
    if (push.buf)
      PyBuffer_Release(&push);
    return PyErr_Format(PyExc_Exception, "unable to submit to Queue");
  }

  // Always wait synchronously
  Py_BEGIN_ALLOW_THREADS;
  vkWaitForFences(self->py_device->device, 1, &self->dispatch_fence, VK_TRUE,
                  UINT64_MAX);
  vkResetFences(self->py_device->device, 1, &self->dispatch_fence);
  Py_END_ALLOW_THREADS;

  if (push.buf)
    PyBuffer_Release(&push);
  Py_RETURN_NONE;
}

/* ----------------------------------------------------------------------------
   vulkan_Compute_bind_cbv
   ------------------------------------------------------------------------- */
PyObject *vulkan_Compute_bind_cbv(vulkan_Compute *self, PyObject *args) {
  uint32_t index;
  PyObject *resource_obj;
  if (!PyArg_ParseTuple(args, "IO", &index, &resource_obj))
    return NULL;

  if (!self->bindless) {
    return PyErr_Format(PyExc_ValueError,
                        "Compute pipeline is not in bindless mode");
  }

  if (!PyObject_TypeCheck(resource_obj, &vulkan_Resource_Type))
    return PyErr_Format(PyExc_ValueError, "Expected a Resource object");

  vulkan_Resource *res = (vulkan_Resource *)resource_obj;
  if (!res->buffer)
    return PyErr_Format(PyExc_ValueError, "Expected a Buffer object");

  if (index >= self->bindless)
    return PyErr_Format(PyExc_ValueError, "Invalid bind index %u (max: %u)",
                        index, self->bindless - 1);

  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = self->descriptor_set;
  write.dstBinding = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo = &res->descriptor_buffer_info;

  vkUpdateDescriptorSets(self->py_device->device, 1, &write, 0, NULL);

  Py_INCREF(resource_obj);
  PyList_SetItem(self->py_cbv_list, index, resource_obj);

  Py_RETURN_NONE;
}

/* ----------------------------------------------------------------------------
   vulkan_Compute_bind_srv
   ------------------------------------------------------------------------- */
PyObject *vulkan_Compute_bind_srv(vulkan_Compute *self, PyObject *args) {
  uint32_t index;
  PyObject *resource_obj;
  if (!PyArg_ParseTuple(args, "IO", &index, &resource_obj))
    return NULL;

  if (!self->bindless) {
    return PyErr_Format(PyExc_ValueError,
                        "Compute pipeline is not in bindless mode");
  }

  if (!PyObject_TypeCheck(resource_obj, &vulkan_Resource_Type))
    return PyErr_Format(PyExc_ValueError, "Expected a Resource object");

  vulkan_Resource *res = (vulkan_Resource *)resource_obj;

  VkDescriptorType type;
  if (res->buffer) {
    type = res->buffer_view ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
                            : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  } else {
    type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  }

  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = self->descriptor_set;
  write.dstBinding = 1024 + index;
  write.descriptorCount = 1;
  write.descriptorType = type;
  if (res->buffer) {
    if (res->buffer_view)
      write.pTexelBufferView = &res->buffer_view;
    else
      write.pBufferInfo = &res->descriptor_buffer_info;
  } else {
    write.pImageInfo = &res->descriptor_image_info;
  }

  vkUpdateDescriptorSets(self->py_device->device, 1, &write, 0, NULL);

  Py_INCREF(resource_obj);
  PyList_SetItem(self->py_srv_list, index, resource_obj);

  Py_RETURN_NONE;
}

/* ----------------------------------------------------------------------------
   vulkan_Compute_bind_uav
   ------------------------------------------------------------------------- */
PyObject *vulkan_Compute_bind_uav(vulkan_Compute *self, PyObject *args) {
  uint32_t index;
  PyObject *resource_obj;
  if (!PyArg_ParseTuple(args, "IO", &index, &resource_obj))
    return NULL;

  if (!self->bindless) {
    return PyErr_Format(PyExc_ValueError,
                        "Compute pipeline is not in bindless mode");
  }

  if (!PyObject_TypeCheck(resource_obj, &vulkan_Resource_Type))
    return PyErr_Format(PyExc_ValueError, "Expected a Resource object");

  vulkan_Resource *res = (vulkan_Resource *)resource_obj;

  VkDescriptorType type;
  if (res->buffer) {
    type = res->buffer_view ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  } else {
    type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }

  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = self->descriptor_set;
  write.dstBinding = 2048 + index;
  write.descriptorCount = 1;
  write.descriptorType = type;
  if (res->buffer) {
    if (res->buffer_view)
      write.pTexelBufferView = &res->buffer_view;
    else
      write.pBufferInfo = &res->descriptor_buffer_info;
  } else {
    write.pImageInfo = &res->descriptor_image_info;
  }

  vkUpdateDescriptorSets(self->py_device->device, 1, &write, 0, NULL);

  Py_INCREF(resource_obj);
  PyList_SetItem(self->py_uav_list, index, resource_obj);

  Py_RETURN_NONE;
}

/* ----------------------------------------------------------------------------
   Method table
   ------------------------------------------------------------------------- */
PyMethodDef vulkan_Compute_methods[] = {
    {"dispatch", (PyCFunction)vulkan_Compute_dispatch, METH_VARARGS,
     "Execute a Compute Pipeline"},
    {"dispatch_indirect", (PyCFunction)vulkan_Compute_dispatch_indirect,
     METH_VARARGS, "Execute an Indirect Compute Pipeline"},
    {"bind_cbv", (PyCFunction)vulkan_Compute_bind_cbv, METH_VARARGS,
     "Bind a CBV to a Bindless Compute Pipeline"},
    {"bind_srv", (PyCFunction)vulkan_Compute_bind_srv, METH_VARARGS,
     "Bind an SRV to a Bindless Compute Pipeline"},
    {"bind_uav", (PyCFunction)vulkan_Compute_bind_uav, METH_VARARGS,
     "Bind an UAV to a Bindless Compute Pipeline"},
    {NULL, NULL, 0, NULL}};
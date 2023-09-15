list(APPEND VKD3DIDL
    vkd3d_d3d12.idl
    vkd3d_d3d12sdklayers.idl
    vkd3d_d3dcommon.idl
    vkd3d_dxgi.idl
    vkd3d_d3dx9shader.idl
    vkd3d_dxgi1_2.idl
    vkd3d_dxgi1_3.idl
    vkd3d_dxgi1_4.idl
    vkd3d_dxgibase.idl
    vkd3d_dxgiformat.idl
    vkd3d_dxgitype.idl
    vkd3d_unknown.idl)

add_idl_headers(vkd3d_idl_headers ${VKD3DIDL})
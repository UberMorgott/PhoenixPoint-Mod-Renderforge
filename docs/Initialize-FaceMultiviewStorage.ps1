# Configure this PowerShell process and its future children only.
# No downloads, installations, application launches, or global environment changes.
[CmdletBinding()]
param()

$rfStorageRoot = 'D:\RenderforgeTools\face-multiview'
$rfStoragePaths = [ordered]@{
    TEMP = 'temp'
    TMP = 'temp'
    PIP_CACHE_DIR = 'cache\pip'
    UV_CACHE_DIR = 'cache\uv'
    UV_PYTHON_CACHE_DIR = 'cache\python-downloads'
    UV_PYTHON_INSTALL_DIR = 'python'
    UV_PYTHON_BIN_DIR = 'bin'
    UV_TOOL_DIR = 'uv-tools'
    UV_TOOL_BIN_DIR = 'bin'
    UV_PROJECT_ENVIRONMENT = 'environments\forge-neo'
    HF_HOME = 'cache\huggingface'
    HF_HUB_CACHE = 'cache\huggingface\hub'
    HF_XET_CACHE = 'cache\huggingface\xet'
    HF_ASSETS_CACHE = 'cache\huggingface\assets'
    TORCH_HOME = 'cache\torch'
    TORCH_EXTENSIONS_DIR = 'cache\torch-extensions'
    TORCHINDUCTOR_CACHE_DIR = 'cache\torch-inductor'
    CUDA_CACHE_PATH = 'cache\cuda'
    GRADIO_TEMP_DIR = 'temp\gradio'
    GRADIO_EXAMPLES_CACHE = 'cache\gradio-examples'
}

foreach ($rfRelativePath in @($rfStoragePaths.Values) + @('downloads', 'models', 'projects', 'logs')) {
    [IO.Directory]::CreateDirectory((Join-Path $rfStorageRoot $rfRelativePath)) | Out-Null
}
foreach ($rfStorageEntry in $rfStoragePaths.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable(
        $rfStorageEntry.Key,
        (Join-Path $rfStorageRoot $rfStorageEntry.Value),
        [EnvironmentVariableTarget]::Process)
}

[pscustomobject]@{
    Root = $rfStorageRoot
    Downloads = Join-Path $rfStorageRoot 'downloads'
    Models = Join-Path $rfStorageRoot 'models'
    PythonEnvironment = Join-Path $rfStorageRoot 'environments\forge-neo'
    Executable = Join-Path $rfStorageRoot 'StableProjectorz-2.4.5\Stable Projectorz 2.4.5.exe'
    WorkingDirectory = Join-Path $rfStorageRoot 'StableProjectorz-2.4.5'
    Scope = 'Current process and future child processes; close this shell to undo'
}

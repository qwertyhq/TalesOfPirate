namespace Corsairs.Platform.Shared.Hosting

open System
open Microsoft.Extensions.Configuration
open Microsoft.Extensions.DependencyInjection
open Microsoft.Extensions.Logging
open Microsoft.Extensions.Hosting
open Microsoft.AspNetCore.Builder
open Corsairs.Platform.Shared.Logging

/// Базовый хост для всех серверов.
[<RequireQualifiedAccess>]
module ServerHost =

    /// Запустить хост (async).
    let runAsync (builder: IHostBuilder) =
        task {
            let host = builder.Build()
            do! host.RunAsync()
        }

    /// Создать WebApplicationBuilder с gRPC поддержкой.
    let createWebBuilder (args: string[]) =
        // Обязательно до CreateBuilder: он сам подключает appsettings.json со
        // слежением за файлом, а ConfigurationManager строит провайдеры сразу
        // при добавлении источника — позже отключать уже поздно, watcher к тому
        // моменту работает. Ключ hostBuilder:reloadConfigOnChange читается из
        // переменных окружения с префиксом DOTNET_, двойное подчёркивание
        // заменяет двоеточие. Причина — в комментарии к AddJsonFile ниже.
        Environment.SetEnvironmentVariable("DOTNET_hostBuilder__reloadConfigOnChange", "false")

        let builder = WebApplication.CreateBuilder(args)
        builder.Configuration
            .SetBasePath(AppContext.BaseDirectory)
            // reloadOnChange = false намеренно. Горячая перезагрузка ставит
            // FileSystemWatcher на каталог и переподписывается на токен
            // изменения после каждого срабатывания. На Windows watcher опирается
            // на ReadDirectoryChangesW и молчит, пока файл не тронут; на macOS и
            // Linux он реализован через FSEvents/kqueue, которые отдают события
            // по каталогу шире — перерегистрация замыкается в бесконечную
            // рекурсию, и процесс виснет в главном потоке ещё до инициализации
            // логирования. Параметры сервера читаются на старте, перечитывать их
            // на лету всё равно незачем.
            .AddJsonFile("appsettings.json", optional = false, reloadOnChange = false)
            .AddJsonFile("appsettings.local.json", optional = true)
            .AddEnvironmentVariables("CORSAIRS_")
        |> ignore
        builder.Logging.ClearProviders() |> ignore
        NLogSetup.configure builder.Host |> ignore
        builder

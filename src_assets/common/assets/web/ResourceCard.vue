<template>
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <br>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-secondary m-1" href="https://github.com/junopark00/hermit-shell/tree/main/docs" target="_blank">
                    {{ $t('resource_card.shell_docs') }} <i class="fas fa-fw fa-arrow-up-right-from-square"></i></a>
                <a class="btn btn-secondary m-1" href="https://github.com/junopark00/hermit-shell/releases" target="_blank">
                    {{ $t('resource_card.shell_releases') }} <i class="fas fa-fw fa-arrow-up-right-from-square"></i></a>
            </div>
        </div>
    </div>
    <!-- Legal -->
    <div class="card p-2 mt-4">
        <div class="card-body">
            <h2>{{ $t('resource_card.legal') }}</h2>
            <br>
            <p>{{ $t('resource_card.legal_desc') }}</p>
            <div class="card-group p-4 align-items-center">
                <a class="btn btn-secondary m-1" href="https://github.com/junopark00/hermit-shell/blob/main/LICENSE"
                    target="_blank">
                    {{ $t('resource_card.license') }} <i class="fas fa-fw fa-arrow-up-right-from-square"></i></a>
                <a class="btn btn-secondary m-1" href="https://github.com/junopark00/hermit-shell/blob/main/NOTICE"
                    target="_blank">
                    {{ $t('resource_card.third_party_notice') }} <i class="fas fa-fw fa-arrow-up-right-from-square"></i></a>
            </div>
        </div>
    </div>
</template>

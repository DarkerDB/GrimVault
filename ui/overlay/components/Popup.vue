<template>
  <div>
    <div
      class="absolute bottom-0 left-0 right-0 top-0 bg-[url('@assets/images/Common_PopupFog.png')] bg-cover bg-center opacity-90"
    ></div>

    <div class="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2">
      <div
        class="text-tan-300 popup mx-auto flex items-center justify-center p-12 text-[26px]"
      >
        <slot></slot>
      </div>
    </div>
  </div>
</template>

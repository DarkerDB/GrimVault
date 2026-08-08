<script setup>
const props = defineProps({
  show: {
    type: Boolean,
    default: false
  }
});
</script>

<template>
  <transition
    enter-active-class="transition-opacity ease-out duration-200"
    enter-from-class="opacity-0 scale-90"
    enter-to-class="opacity-100 scale-100"
    leave-active-class="transition-opacity ease-in duration-200"
    leave-from-class="opacity-100 scale-100"
    leave-to-class="opacity-0 scale-90"
  >
    <div v-if="show" id="tooltip">
      <div class="tooltip-overlay"></div>
      <div class="tooltip-content">
        <div class="spinner-wrapper">
          <img
            src="@assets/images/Loading_Img.png"
            alt="Loading..."
            class="spinner-image"
          />
        </div>
      </div>
    </div>
  </transition>
</template>

<style scoped>
.spinner-wrapper {
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 20px;
}

.spinner-image {
  width: 48px;
  height: 48px;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  from {
    transform: rotate(0deg);
  }
  to {
    transform: rotate(360deg);
  }
}
</style>

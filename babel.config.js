module.exports = function babelConfig(api) {
  api.cache(true);

  return {
    presets: ['babel-preset-expo'],
    plugins: ['@tamagui/babel-plugin'],
  };
};
